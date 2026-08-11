#include "corsair_hid.h"
#include "nvidia_temp.h"
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define APP_TITLE L"Corsair NVIDIA Fan Control"
#define APP_RUN_VALUE L"CorsairNvidiaFanControl"
#define LEGACY_APP_RUN_VALUE L"CorsairFanControl"
#define SETTINGS_KEY L"Software\\CorsairNvidiaFanControl"
#define LEGACY_SETTINGS_KEY L"Software\\CorsairFanControl"
#define STARTUP_APPROVED_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"

#define IDC_DEVICE_COMBO 100
#define IDC_SCAN 101
#define IDC_REFRESH 103
#define IDC_APPLY_ALL 104
#define IDC_STATUS 105
#define IDC_DUTY_BASE 200
#define IDC_DUTY_LABEL_BASE 300
#define IDC_APPLY_BASE 400
#define IDC_MODE_BASE 500
#define IDC_RPM_BASE 600
#define IDC_TEMP_BASE 700
#define IDC_VOLT_BASE 800
#define IDC_GPU_STATUS 900
#define IDC_GPU_TEMP_LOW 901
#define IDC_GPU_DUTY_LOW 902
#define IDC_GPU_TEMP_HIGH 903
#define IDC_GPU_DUTY_HIGH 904
#define IDC_AUTOSTART 905
#define IDC_SAVE_SETTINGS 906
#define REFRESH_TIMER 1
#define TRAY_RETRY_TIMER 2
#define POLL_INTERVAL_MS 5000
#define TRAY_RETRY_INTERVAL_MS 2000
#define WM_TRAYICON (WM_APP + 1)
#define TRAY_ICON_ID 1
#define IDM_TRAY_OPEN 1000
#define IDM_TRAY_EXIT 1001

#define FAN_MODE_DETECTED_INDEX 0
#define FAN_MODE_PWM_INDEX 1
#define FAN_MODE_DC_INDEX 2
#define FAN_MODE_OFF_INDEX 3
#define FAN_MODE_NVIDIA_INDEX 4

typedef struct FanSettings {
    int mode_index;
    int duty_percent;
    bool apply;
} FanSettings;

typedef struct ControllerRuntime {
    CorsairDevice device;
    FanSettings fan[CORSAIR_FAN_COUNT];
    int last_nvidia_duty[CORSAIR_FAN_COUNT];
    char last_error[256];
    bool opened;
} ControllerRuntime;

typedef struct AppState {
    HWND hwnd;
    HWND device_combo;
    HWND scan_btn;
    HWND refresh_btn;
    HWND apply_all_btn;
    HWND status_label;
    HWND fan_mode[CORSAIR_FAN_COUNT];
    HWND fan_rpm[CORSAIR_FAN_COUNT];
    HWND fan_slider[CORSAIR_FAN_COUNT];
    HWND fan_duty[CORSAIR_FAN_COUNT];
    HWND fan_apply[CORSAIR_FAN_COUNT];
    HWND temp_label[CORSAIR_TEMP_COUNT];
    HWND volt_label[CORSAIR_VOLT_COUNT];
    HWND gpu_status_label;
    HWND gpu_temp_low_edit;
    HWND gpu_duty_low_edit;
    HWND gpu_temp_high_edit;
    HWND gpu_duty_high_edit;
    HWND autostart_checkbox;
    HWND save_btn;
    CorsairDeviceInfo devices[CORSAIR_MAX_DEVICES];
    int device_count;
    int saved_device_index;
    int active_device_index;
    wchar_t last_device_key[32];
    ControllerRuntime controllers[CORSAIR_MAX_DEVICES];
    NvidiaGpuStatus gpu;
    bool gpu_ok;
    UINT taskbar_created_msg;
    bool start_in_tray;
    bool tray_added;
    bool tray_icon_wanted;
    bool allow_close;
} AppState;

static AppState g_app;

static int selected_device_index(AppState *app);
static int edit_int(HWND edit, int fallback);
static int clamp_int(int value, int min_value, int max_value);
static int slider_duty(AppState *app, int fan);
static void update_duty_label(AppState *app, int fan);
static bool apply_one(AppState *app, int device_index, int fan, char *err, size_t err_len);
static bool apply_saved_fan_settings(AppState *app, int device_index, char *err, size_t err_len);
static void save_device_settings_for_index(AppState *app, int device_index);
static void open_selected_device(AppState *app);
static void refresh_status(AppState *app);
static void clear_device_status_view(AppState *app);

static void copy_wstr(wchar_t *dst, size_t dst_count, const wchar_t *src)
{
    if (!dst || dst_count == 0) {
        return;
    }
    if (!src) {
        dst[0] = L'\0';
        return;
    }

    size_t i = 0;
    for (; i + 1 < dst_count && src[i] != L'\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = L'\0';
}

static bool get_exe_path(wchar_t *path, DWORD path_len)
{
    DWORD len = GetModuleFileNameW(NULL, path, path_len);
    return len > 0 && len < path_len;
}

static bool autostart_command(wchar_t *cmd, DWORD cmd_len)
{
    wchar_t path[MAX_PATH];
    if (!get_exe_path(path, (DWORD)(sizeof(path) / sizeof(path[0])))) {
        return false;
    }

    int written = swprintf(cmd, cmd_len, L"\"%s\" --tray", path);
    return written > 0 && (DWORD)written < cmd_len;
}

static bool startup_approved_allows(void)
{
    HKEY key;
    BYTE value[16] = { 0 };
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    bool allowed = true;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_APPROVED_RUN_KEY,
                      0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return true;
    }

    if (RegQueryValueExW(key, APP_RUN_VALUE, NULL, &type, value, &bytes) == ERROR_SUCCESS &&
        type == REG_BINARY && bytes > 0) {
        allowed = value[0] != 0x03;
    }

    RegCloseKey(key);
    return allowed;
}

static void set_startup_approved_enabled(bool enabled)
{
    HKEY key;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, STARTUP_APPROVED_RUN_KEY,
                              0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (rc != ERROR_SUCCESS) {
        return;
    }

    if (enabled) {
        const BYTE value[12] = { 0x02, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0 };
        RegSetValueExW(key, APP_RUN_VALUE, 0, REG_BINARY, value, sizeof(value));
        RegDeleteValueW(key, LEGACY_APP_RUN_VALUE);
    } else {
        RegDeleteValueW(key, APP_RUN_VALUE);
        RegDeleteValueW(key, LEGACY_APP_RUN_VALUE);
    }

    RegCloseKey(key);
}

static bool is_autostart_enabled(void)
{
    HKEY key;
    wchar_t value[1024];
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    wchar_t expected[1024];
    bool enabled = false;

    if (!autostart_command(expected, (DWORD)(sizeof(expected) / sizeof(expected[0])))) {
        return false;
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }

    if (RegQueryValueExW(key, APP_RUN_VALUE, NULL, &type, (LPBYTE)value, &bytes) == ERROR_SUCCESS &&
        type == REG_SZ) {
        value[(sizeof(value) / sizeof(value[0])) - 1] = L'\0';
        enabled = wcscmp(value, expected) == 0 && startup_approved_allows();
    }

    RegCloseKey(key);
    return enabled;
}

static bool set_autostart_enabled(bool enabled)
{
    HKEY key;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER,
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                              0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (rc != ERROR_SUCCESS) {
        return false;
    }

    if (enabled) {
        wchar_t cmd[1024];
        if (!autostart_command(cmd, (DWORD)(sizeof(cmd) / sizeof(cmd[0])))) {
            RegCloseKey(key);
            return false;
        }
        rc = RegSetValueExW(key, APP_RUN_VALUE, 0, REG_SZ,
                            (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
        RegDeleteValueW(key, LEGACY_APP_RUN_VALUE);
        if (rc == ERROR_SUCCESS) {
            set_startup_approved_enabled(true);
        }
    } else {
        rc = RegDeleteValueW(key, APP_RUN_VALUE);
        if (rc == ERROR_FILE_NOT_FOUND) {
            rc = ERROR_SUCCESS;
        }
        RegDeleteValueW(key, LEGACY_APP_RUN_VALUE);
        set_startup_approved_enabled(false);
    }

    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

static bool settings_open_existing_key(const wchar_t *path, HKEY *key, REGSAM access)
{
    return RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, access, key) == ERROR_SUCCESS;
}

static bool settings_create_key(const wchar_t *path, HKEY *key, REGSAM access)
{
    return RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, access, NULL, key, NULL) == ERROR_SUCCESS;
}

static bool settings_open_root_key(HKEY *key, REGSAM access)
{
    return settings_create_key(SETTINGS_KEY, key, access);
}

static DWORD settings_read_dword(HKEY key, const wchar_t *name, DWORD fallback)
{
    DWORD value = 0;
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    if (RegQueryValueExW(key, name, NULL, &type, (LPBYTE)&value, &bytes) == ERROR_SUCCESS && type == REG_DWORD) {
        return value;
    }
    return fallback;
}

static bool settings_try_read_dword(HKEY key, const wchar_t *name, DWORD *value)
{
    DWORD type = 0;
    DWORD bytes = sizeof(*value);
    return RegQueryValueExW(key, name, NULL, &type, (LPBYTE)value, &bytes) == ERROR_SUCCESS &&
           type == REG_DWORD;
}

static void settings_write_dword(HKEY key, const wchar_t *name, DWORD value)
{
    RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
}

static bool settings_read_string(HKEY key, const wchar_t *name, wchar_t *value, DWORD value_count)
{
    DWORD type = 0;
    DWORD bytes = value_count * sizeof(wchar_t);
    if (value_count == 0) {
        return false;
    }

    if (RegQueryValueExW(key, name, NULL, &type, (LPBYTE)value, &bytes) == ERROR_SUCCESS &&
        type == REG_SZ) {
        value[value_count - 1] = L'\0';
        return true;
    }

    value[0] = L'\0';
    return false;
}

static void settings_write_string(HKEY key, const wchar_t *name, const wchar_t *value)
{
    RegSetValueExW(key, name, 0, REG_SZ,
                   (const BYTE *)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
}

static void device_settings_key(const CorsairDeviceInfo *info, wchar_t *key, size_t key_count)
{
    uint64_t hash = 1469598103934665603ULL;
    const wchar_t *text = info && info->path[0] ? info->path : L"unknown";

    for (size_t i = 0; text[i] != L'\0'; ++i) {
        hash ^= (uint64_t)(uint16_t)text[i];
        hash *= 1099511628211ULL;
    }

    swprintf(key, key_count, L"%016llX", (unsigned long long)hash);
}

static bool settings_open_device_key_for_index(AppState *app, int device_index, HKEY *key, REGSAM access)
{
    if (device_index < 0 || device_index >= app->device_count) {
        return false;
    }

    wchar_t device_key[32];
    wchar_t path[160];
    device_settings_key(&app->devices[device_index], device_key, sizeof(device_key) / sizeof(device_key[0]));
    swprintf(path, sizeof(path) / sizeof(path[0]), SETTINGS_KEY L"\\Devices\\%s", device_key);

    if (access & (KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE | WRITE_DAC | WRITE_OWNER)) {
        return settings_create_key(path, key, access);
    }
    return settings_open_existing_key(path, key, access);
}

static bool settings_open_legacy_device_key_for_index(AppState *app, int device_index, HKEY *key, REGSAM access)
{
    if (device_index < 0 || device_index >= app->device_count) {
        return false;
    }

    wchar_t device_key[32];
    wchar_t path[160];
    device_settings_key(&app->devices[device_index], device_key, sizeof(device_key) / sizeof(device_key[0]));
    swprintf(path, sizeof(path) / sizeof(path[0]), LEGACY_SETTINGS_KEY L"\\Devices\\%s", device_key);
    return settings_open_existing_key(path, key, access);
}

static int find_device_by_key(AppState *app, const wchar_t *key)
{
    if (!key || key[0] == L'\0') {
        return -1;
    }

    for (int i = 0; i < app->device_count; ++i) {
        wchar_t current[32];
        device_settings_key(&app->devices[i], current, sizeof(current) / sizeof(current[0]));
        if (wcscmp(current, key) == 0) {
            return i;
        }
    }

    return -1;
}

static void set_edit_int(HWND edit, int value)
{
    wchar_t text[32];
    swprintf(text, sizeof(text) / sizeof(text[0]), L"%d", value);
    SetWindowTextW(edit, text);
}

static void reset_controller_settings(ControllerRuntime *controller)
{
    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        controller->fan[i].mode_index = FAN_MODE_DETECTED_INDEX;
        controller->fan[i].duty_percent = 50;
        controller->fan[i].apply = false;
        controller->last_nvidia_duty[i] = -1;
    }
    controller->last_error[0] = '\0';
}

static void show_device_settings(AppState *app, int device_index)
{
    if (device_index < 0 || device_index >= app->device_count) {
        return;
    }

    ControllerRuntime *controller = &app->controllers[device_index];
    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        SendMessageW(app->fan_mode[i], CB_SETCURSEL, controller->fan[i].mode_index, 0);
        SendMessageW(app->fan_slider[i], TBM_SETPOS, TRUE, controller->fan[i].duty_percent);
        update_duty_label(app, i);
    }
}

static void capture_device_settings(AppState *app, int device_index)
{
    if (device_index < 0 || device_index >= app->device_count ||
        device_index != app->active_device_index) {
        return;
    }

    ControllerRuntime *controller = &app->controllers[device_index];
    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        LRESULT mode = SendMessageW(app->fan_mode[i], CB_GETCURSEL, 0, 0);
        controller->fan[i].mode_index =
            mode == CB_ERR ? FAN_MODE_DETECTED_INDEX : clamp_int((int)mode, 0, FAN_MODE_NVIDIA_INDEX);
        controller->fan[i].duty_percent = clamp_int(slider_duty(app, i), 0, 100);
    }
}

static bool load_fan_settings_from_key(ControllerRuntime *controller, HKEY key)
{
    bool any = false;

    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        wchar_t name[32];
        DWORD mode = FAN_MODE_DETECTED_INDEX;

        swprintf(name, sizeof(name) / sizeof(name[0]), L"FanMode%d", i);
        bool mode_exists = settings_try_read_dword(key, name, &mode);
        if (mode_exists && mode <= FAN_MODE_NVIDIA_INDEX) {
            controller->fan[i].mode_index = (int)mode;
            any = true;
        }

        swprintf(name, sizeof(name) / sizeof(name[0]), L"FanDuty%d", i);
        DWORD duty_value = 50;
        bool duty_exists = settings_try_read_dword(key, name, &duty_value);
        if (duty_exists) {
            controller->fan[i].duty_percent = clamp_int((int)duty_value, 0, 100);
            any = true;
        }

        swprintf(name, sizeof(name) / sizeof(name[0]), L"FanApply%d", i);
        DWORD apply_value = 0;
        bool apply_exists = settings_try_read_dword(key, name, &apply_value);
        if (apply_exists) {
            controller->fan[i].apply = apply_value != 0;
            any = true;
        } else if (mode_exists || duty_exists) {
            controller->fan[i].apply =
                mode == FAN_MODE_NVIDIA_INDEX || mode == FAN_MODE_OFF_INDEX || duty_exists;
        }
    }

    return any;
}

static void load_global_settings(AppState *app)
{
    HKEY key;
    if (!settings_open_existing_key(SETTINGS_KEY, &key, KEY_QUERY_VALUE) &&
        !settings_open_existing_key(LEGACY_SETTINGS_KEY, &key, KEY_QUERY_VALUE)) {
        app->saved_device_index = 0;
        app->last_device_key[0] = L'\0';
        return;
    }

    app->saved_device_index = (int)settings_read_dword(key, L"DeviceIndex", 0);
    settings_read_string(key, L"LastDeviceKey", app->last_device_key,
                         (DWORD)(sizeof(app->last_device_key) / sizeof(app->last_device_key[0])));
    set_edit_int(app->gpu_temp_low_edit, (int)settings_read_dword(key, L"GpuTempLow", 40));
    set_edit_int(app->gpu_duty_low_edit, (int)settings_read_dword(key, L"GpuDutyLow", 25));
    set_edit_int(app->gpu_temp_high_edit, (int)settings_read_dword(key, L"GpuTempHigh", 80));
    set_edit_int(app->gpu_duty_high_edit, (int)settings_read_dword(key, L"GpuDutyHigh", 100));

    RegCloseKey(key);
}

static void load_device_settings(AppState *app, int device_index)
{
    HKEY key;
    ControllerRuntime *controller = &app->controllers[device_index];
    reset_controller_settings(controller);

    bool loaded = false;
    if (settings_open_device_key_for_index(app, device_index, &key, KEY_QUERY_VALUE)) {
        loaded = load_fan_settings_from_key(controller, key);
        RegCloseKey(key);
    }
    if (!loaded && settings_open_legacy_device_key_for_index(app, device_index, &key, KEY_QUERY_VALUE)) {
        loaded = load_fan_settings_from_key(controller, key);
        RegCloseKey(key);
    }

    if (!loaded && settings_open_existing_key(LEGACY_SETTINGS_KEY, &key, KEY_QUERY_VALUE)) {
        int legacy_device_index = (int)settings_read_dword(key, L"DeviceIndex", -1);
        if (legacy_device_index == device_index) {
            load_fan_settings_from_key(controller, key);
        }
        RegCloseKey(key);
    }
}

static void save_global_settings(AppState *app)
{
    HKEY key;
    if (!settings_open_root_key(&key, KEY_SET_VALUE)) {
        return;
    }

    int device_index = selected_device_index(app);
    if (device_index >= 0) {
        wchar_t device_key[32];
        device_settings_key(&app->devices[device_index], device_key,
                            sizeof(device_key) / sizeof(device_key[0]));
        settings_write_dword(key, L"DeviceIndex", (DWORD)device_index);
        settings_write_string(key, L"LastDeviceKey", device_key);
        copy_wstr(app->last_device_key, sizeof(app->last_device_key) / sizeof(app->last_device_key[0]), device_key);
        app->saved_device_index = device_index;
    }

    settings_write_dword(key, L"GpuTempLow", (DWORD)clamp_int(edit_int(app->gpu_temp_low_edit, 40), 0, 120));
    settings_write_dword(key, L"GpuDutyLow", (DWORD)clamp_int(edit_int(app->gpu_duty_low_edit, 25), 0, 100));
    settings_write_dword(key, L"GpuTempHigh", (DWORD)clamp_int(edit_int(app->gpu_temp_high_edit, 80), 0, 120));
    settings_write_dword(key, L"GpuDutyHigh", (DWORD)clamp_int(edit_int(app->gpu_duty_high_edit, 100), 0, 100));

    RegCloseKey(key);
}

static void save_device_settings_for_index(AppState *app, int device_index)
{
    if (device_index < 0 || device_index >= app->device_count) {
        return;
    }

    capture_device_settings(app, device_index);

    HKEY key;
    if (!settings_open_device_key_for_index(app, device_index, &key, KEY_SET_VALUE)) {
        return;
    }

    ControllerRuntime *controller = &app->controllers[device_index];
    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        wchar_t name[32];
        swprintf(name, sizeof(name) / sizeof(name[0]), L"FanMode%d", i);
        settings_write_dword(key, name, (DWORD)controller->fan[i].mode_index);

        swprintf(name, sizeof(name) / sizeof(name[0]), L"FanDuty%d", i);
        settings_write_dword(key, name, (DWORD)controller->fan[i].duty_percent);

        swprintf(name, sizeof(name) / sizeof(name[0]), L"FanApply%d", i);
        settings_write_dword(key, name, controller->fan[i].apply ? 1u : 0u);
    }

    RegCloseKey(key);
}

static void save_settings(AppState *app)
{
    int device_index = selected_device_index(app);
    save_global_settings(app);
    if (device_index >= 0) {
        save_device_settings_for_index(app, device_index);
    }
}

static bool tray_update(AppState *app, DWORD message)
{
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = app->hwnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!nid.hIcon) {
        nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    }
    copy_wstr(nid.szTip, sizeof(nid.szTip) / sizeof(nid.szTip[0]), L"Corsair NVIDIA Fan Control");

    if (Shell_NotifyIconW(message, &nid)) {
        app->tray_added = message != NIM_DELETE;
        return true;
    }
    return false;
}

static void ensure_tray_icon(AppState *app)
{
    if (app->tray_added) {
        KillTimer(app->hwnd, TRAY_RETRY_TIMER);
        return;
    }

    if (tray_update(app, NIM_ADD)) {
        KillTimer(app->hwnd, TRAY_RETRY_TIMER);
    } else {
        SetTimer(app->hwnd, TRAY_RETRY_TIMER, TRAY_RETRY_INTERVAL_MS, NULL);
    }
}

static void hide_to_tray(AppState *app)
{
    app->tray_icon_wanted = true;
    if (!app->tray_added) {
        ensure_tray_icon(app);
    }
    ShowWindow(app->hwnd, SW_HIDE);
}

static void show_from_tray(AppState *app)
{
    ShowWindow(app->hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(app->hwnd);
}

static void show_tray_menu(AppState *app)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN, L"Open");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(app->hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, app->hwnd, NULL);
    DestroyMenu(menu);
}

static void set_status(AppState *app, const wchar_t *text)
{
    SetWindowTextW(app->status_label, text);
}

static void set_status_from_error(AppState *app, const char *prefix, const char *err)
{
    wchar_t wbuf[512];
    char buf[512];
    snprintf(buf, sizeof(buf), "%s: %s", prefix, err && err[0] ? err : "Unknown error");
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    SetWindowTextW(app->status_label, wbuf);
}

static int selected_device_index(AppState *app)
{
    LRESULT index = SendMessageW(app->device_combo, CB_GETCURSEL, 0, 0);
    if (index == CB_ERR || index < 0 || index >= app->device_count) {
        return -1;
    }
    return (int)index;
}

static int slider_duty(AppState *app, int fan)
{
    return (int)SendMessageW(app->fan_slider[fan], TBM_GETPOS, 0, 0);
}

static CorsairFanMode configured_mode(ControllerRuntime *controller, int fan)
{
    switch (controller->fan[fan].mode_index) {
    case FAN_MODE_PWM_INDEX:
        return CORSAIR_FAN_PWM;
    case FAN_MODE_DC_INDEX:
        return CORSAIR_FAN_DC;
    case FAN_MODE_OFF_INDEX:
        return CORSAIR_FAN_DISCONNECTED;
    default:
        return (CorsairFanMode)controller->device.status.fan_mode[fan];
    }
}

static bool fan_uses_nvidia_curve(const ControllerRuntime *controller, int fan)
{
    return controller->fan[fan].mode_index == FAN_MODE_NVIDIA_INDEX;
}

static int edit_int(HWND edit, int fallback)
{
    wchar_t text[32];
    wchar_t *end = NULL;
    long value;

    if (GetWindowTextW(edit, text, (int)(sizeof(text) / sizeof(text[0]))) <= 0) {
        return fallback;
    }

    value = wcstol(text, &end, 10);
    if (end == text) {
        return fallback;
    }
    return (int)value;
}

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static int nvidia_curve_duty(AppState *app)
{
    int temp_low = clamp_int(edit_int(app->gpu_temp_low_edit, 40), 0, 120);
    int duty_low = clamp_int(edit_int(app->gpu_duty_low_edit, 25), 0, 100);
    int temp_high = clamp_int(edit_int(app->gpu_temp_high_edit, 80), 0, 120);
    int duty_high = clamp_int(edit_int(app->gpu_duty_high_edit, 100), 0, 100);
    int temp = app->gpu.temperature_c;

    if (temp_high <= temp_low) {
        return temp >= temp_low ? duty_high : duty_low;
    }
    if (temp <= temp_low) {
        return duty_low;
    }
    if (temp >= temp_high) {
        return duty_high;
    }

    return duty_low + ((temp - temp_low) * (duty_high - duty_low)) / (temp_high - temp_low);
}

static void update_controls_enabled(AppState *app)
{
    BOOL opened = FALSE;
    if (app->active_device_index >= 0 && app->active_device_index < app->device_count) {
        opened = app->controllers[app->active_device_index].opened ? TRUE : FALSE;
    }
    EnableWindow(app->refresh_btn, opened);
    EnableWindow(app->apply_all_btn, opened);
    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        EnableWindow(app->fan_mode[i], opened);
        EnableWindow(app->fan_slider[i], opened);
        EnableWindow(app->fan_apply[i], opened);
    }
}

static void update_duty_label(AppState *app, int fan)
{
    wchar_t text[32];
    swprintf(text, sizeof(text) / sizeof(text[0]), L"%d%%", slider_duty(app, fan));
    SetWindowTextW(app->fan_duty[fan], text);
}

static void update_gpu_status_view(AppState *app, const char *err)
{
    wchar_t text[256];

    if (app->gpu_ok) {
        swprintf(text, sizeof(text) / sizeof(text[0]), L"GPU temp: %d C", app->gpu.temperature_c);
    } else {
        wchar_t werr[160];
        MultiByteToWideChar(CP_UTF8, 0, err && err[0] ? err : "unavailable", -1,
                            werr, (int)(sizeof(werr) / sizeof(werr[0])));
        swprintf(text, sizeof(text) / sizeof(text[0]), L"NVIDIA: %s", werr);
    }

    SetWindowTextW(app->gpu_status_label, text);
}

static void refresh_gpu_status(AppState *app)
{
    char err[256] = { 0 };
    app->gpu_ok = nvidia_temp_read(&app->gpu, err, sizeof(err));
    update_gpu_status_view(app, err);
}

static bool apply_nvidia_curve_to_controller(AppState *app, int device_index,
                                              char *err, size_t err_len)
{
    if (device_index < 0 || device_index >= app->device_count) {
        snprintf(err, err_len, "Invalid controller index.");
        return false;
    }

    ControllerRuntime *controller = &app->controllers[device_index];
    if (!controller->opened || !app->gpu_ok) {
        return true;
    }

    bool ok = true;
    int duty = nvidia_curve_duty(app);
    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        if (!fan_uses_nvidia_curve(controller, i)) {
            controller->last_nvidia_duty[i] = -1;
            continue;
        }

        if (controller->device.status.fan_mode[i] != CORSAIR_FAN_PWM &&
            controller->device.status.fan_mode[i] != CORSAIR_FAN_DC) {
            controller->last_nvidia_duty[i] = -1;
            continue;
        }

        if (controller->last_nvidia_duty[i] == duty) {
            continue;
        }

        char fan_err[256] = { 0 };
        if (!corsair_set_fan_duty(&controller->device, i, duty, fan_err, sizeof(fan_err))) {
            ok = false;
            snprintf(err, err_len, "Fan %d: %s", i + 1, fan_err);
            continue;
        }

        controller->last_nvidia_duty[i] = duty;
        if (device_index == app->active_device_index) {
            SendMessageW(app->fan_slider[i], TBM_SETPOS, TRUE, duty);
            update_duty_label(app, i);
        }
    }

    return ok;
}

static void update_status_view(AppState *app)
{
    wchar_t text[256];
    int device_index = app->active_device_index;

    if (device_index < 0 || device_index >= app->device_count) {
        return;
    }

    ControllerRuntime *controller = &app->controllers[device_index];
    if (!controller->opened) {
        clear_device_status_view(app);
        if (controller->last_error[0]) {
            set_status_from_error(app, "Controller", controller->last_error);
        } else {
            set_status(app, L"Controller is not open.");
        }
        return;
    }

    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 L"%s / %d rpm",
                 corsair_fan_mode_name(controller->device.status.fan_mode[i]),
                 controller->device.status.fan_rpm[i]);
        SetWindowTextW(app->fan_rpm[i], text);
    }

    for (int i = 0; i < CORSAIR_TEMP_COUNT; ++i) {
        if (controller->device.status.temp_connected[i]) {
            swprintf(text, sizeof(text) / sizeof(text[0]), L"T%d: %.1f C", i + 1,
                     controller->device.status.temp_c[i]);
        } else {
            swprintf(text, sizeof(text) / sizeof(text[0]), L"T%d: N/A", i + 1);
        }
        SetWindowTextW(app->temp_label[i], text);
    }

    static const wchar_t *rail_names[CORSAIR_VOLT_COUNT] = { L"+12V", L"+5V", L"+3.3V" };
    for (int i = 0; i < CORSAIR_VOLT_COUNT; ++i) {
        swprintf(text, sizeof(text) / sizeof(text[0]), L"%s: %.2f V", rail_names[i],
                 controller->device.status.volts[i]);
        SetWindowTextW(app->volt_label[i], text);
    }

    swprintf(text, sizeof(text) / sizeof(text[0]), L"Firmware %d.%d.%d, Bootloader %d.%d",
             controller->device.status.firmware[0],
             controller->device.status.firmware[1],
             controller->device.status.firmware[2],
             controller->device.status.bootloader[0],
             controller->device.status.bootloader[1]);
    set_status(app, text);
}

static void clear_device_status_view(AppState *app)
{
    static const wchar_t *rail_names[CORSAIR_VOLT_COUNT] = { L"+12V", L"+5V", L"+3.3V" };
    wchar_t text[64];

    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        SetWindowTextW(app->fan_rpm[i], L"N/A / 0 rpm");
    }
    for (int i = 0; i < CORSAIR_TEMP_COUNT; ++i) {
        swprintf(text, sizeof(text) / sizeof(text[0]), L"T%d: N/A", i + 1);
        SetWindowTextW(app->temp_label[i], text);
    }
    for (int i = 0; i < CORSAIR_VOLT_COUNT; ++i) {
        swprintf(text, sizeof(text) / sizeof(text[0]), L"%s: N/A", rail_names[i]);
        SetWindowTextW(app->volt_label[i], text);
    }
}

static void close_controller(AppState *app, int device_index)
{
    if (device_index < 0 || device_index >= app->device_count) {
        return;
    }

    ControllerRuntime *controller = &app->controllers[device_index];
    if (controller->opened) {
        corsair_close(&controller->device);
        controller->opened = false;
    }
    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        controller->last_nvidia_duty[i] = -1;
    }
}

static void close_all_controllers(AppState *app)
{
    for (int i = 0; i < app->device_count; ++i) {
        close_controller(app, i);
    }
}

static void activate_device_selection(AppState *app, int device_index, bool save_previous)
{
    int previous = app->active_device_index;

    if (device_index < 0 || device_index >= app->device_count) {
        return;
    }

    if (save_previous &&
        previous >= 0 && previous < app->device_count &&
        previous != device_index) {
        save_device_settings_for_index(app, previous);
    }

    SendMessageW(app->device_combo, CB_SETCURSEL, device_index, 0);
    app->active_device_index = device_index;
    show_device_settings(app, device_index);
    clear_device_status_view(app);
    update_controls_enabled(app);
    save_global_settings(app);
    if (app->controllers[device_index].opened) {
        update_status_view(app);
    } else {
        set_status(app, L"Controller selected; opening it now.");
    }
}

static bool apply_saved_fan_settings(AppState *app, int device_index, char *err, size_t err_len)
{
    bool ok = true;
    char last_err[256] = { 0 };

    if (device_index < 0 || device_index >= app->device_count ||
        !app->controllers[device_index].opened) {
        snprintf(err, err_len, "Controller is not initialized.");
        return false;
    }

    ControllerRuntime *controller = &app->controllers[device_index];
    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        if (!controller->fan[i].apply) {
            continue;
        }

        char fan_err[256] = { 0 };
        if (!apply_one(app, device_index, i, fan_err, sizeof(fan_err))) {
            ok = false;
            snprintf(last_err, sizeof(last_err), "Fan %d: %s", i + 1, fan_err);
        }
    }

    if (!ok) {
        snprintf(err, err_len, "%s", last_err[0] ? last_err : "Some fan settings could not be applied.");
    }
    return ok;
}

static bool open_controller(AppState *app, int device_index, char *err, size_t err_len)
{
    if (device_index < 0 || device_index >= app->device_count) {
        snprintf(err, err_len, "Invalid controller index.");
        return false;
    }

    ControllerRuntime *controller = &app->controllers[device_index];
    if (controller->opened) {
        return true;
    }

    controller->last_error[0] = '\0';
    if (!corsair_open(&controller->device, &app->devices[device_index], err, err_len)) {
        snprintf(controller->last_error, sizeof(controller->last_error), "%s", err);
        return false;
    }

    if (!corsair_initialize(&controller->device, err, err_len)) {
        snprintf(controller->last_error, sizeof(controller->last_error), "%s", err);
        corsair_close(&controller->device);
        return false;
    }

    controller->opened = true;
    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        controller->last_nvidia_duty[i] = -1;
    }

    char apply_err[256] = { 0 };
    if (!apply_saved_fan_settings(app, device_index, apply_err, sizeof(apply_err))) {
        snprintf(controller->last_error, sizeof(controller->last_error), "%s", apply_err);
    }

    return true;
}

static void scan_devices(AppState *app)
{
    char err[256] = { 0 };
    wchar_t item[256];

    if (app->active_device_index >= 0 && app->active_device_index < app->device_count) {
        save_device_settings_for_index(app, app->active_device_index);
    }
    save_global_settings(app);
    close_all_controllers(app);

    SendMessageW(app->device_combo, CB_RESETCONTENT, 0, 0);
    ZeroMemory(app->controllers, sizeof(app->controllers));
    app->device_count = corsair_find_devices(app->devices, CORSAIR_MAX_DEVICES, err, sizeof(err));
    app->active_device_index = -1;

    for (int i = 0; i < app->device_count; ++i) {
        load_device_settings(app, i);
        swprintf(item, sizeof(item) / sizeof(item[0]), L"%s (PID %04X)",
                 app->devices[i].model, app->devices[i].product_id);
        SendMessageW(app->device_combo, CB_ADDSTRING, 0, (LPARAM)item);
    }

    if (app->device_count > 0) {
        int selected = find_device_by_key(app, app->last_device_key);
        if (selected < 0) {
            selected = clamp_int(app->saved_device_index, 0, app->device_count - 1);
        }
        activate_device_selection(app, selected, false);
        refresh_gpu_status(app);

        int opened_count = 0;
        int error_count = 0;
        char first_error[256] = { 0 };
        for (int i = 0; i < app->device_count; ++i) {
            char open_err[256] = { 0 };
            if (open_controller(app, i, open_err, sizeof(open_err))) {
                ++opened_count;
                if (app->controllers[i].last_error[0]) {
                    ++error_count;
                    if (!first_error[0]) {
                        snprintf(first_error, sizeof(first_error), "Controller %d: %s",
                                 i + 1, app->controllers[i].last_error);
                    }
                }
            } else {
                ++error_count;
                if (!first_error[0]) {
                    snprintf(first_error, sizeof(first_error), "Controller %d: %s", i + 1, open_err);
                }
            }
        }

        update_controls_enabled(app);
        update_status_view(app);
        if (error_count > 0) {
            set_status_from_error(app, "Not all controllers were updated", first_error);
        } else {
            swprintf(item, sizeof(item) / sizeof(item[0]),
                     L"%d of %d controllers initialized and updated.", opened_count, app->device_count);
            set_status(app, item);
        }
    } else {
        clear_device_status_view(app);
        update_controls_enabled(app);
        set_status_from_error(app, "Scan", err);
    }
}

static void open_selected_device(AppState *app)
{
    char err[256] = { 0 };
    int index = selected_device_index(app);

    if (index < 0) {
        set_status(app, L"No device selected.");
        return;
    }

    if (index != app->active_device_index) {
        activate_device_selection(app, index, true);
    }

    ControllerRuntime *controller = &app->controllers[index];
    if (controller->opened) {
        update_controls_enabled(app);
        update_status_view(app);
        return;
    }

    app->active_device_index = index;
    save_global_settings(app);
    set_status(app, L"Opening selected device...");

    refresh_gpu_status(app);
    if (!open_controller(app, index, err, sizeof(err))) {
        set_status_from_error(app, "Open", err);
        update_controls_enabled(app);
        return;
    }

    update_controls_enabled(app);
    update_status_view(app);
    if (controller->last_error[0]) {
        set_status_from_error(app, "Apply saved", controller->last_error);
    }
}

static void refresh_status(AppState *app)
{
    refresh_gpu_status(app);

    char first_error[256] = { 0 };
    int first_error_index = -1;
    for (int i = 0; i < app->device_count; ++i) {
        ControllerRuntime *controller = &app->controllers[i];
        bool newly_opened = false;
        bool controller_ok = true;
        char err[256] = { 0 };

        if (!controller->opened) {
            if (!open_controller(app, i, err, sizeof(err))) {
                if (first_error_index < 0) {
                    first_error_index = i;
                    snprintf(first_error, sizeof(first_error), "%s", err);
                }
                continue;
            }
            newly_opened = true;
            if (controller->last_error[0]) {
                controller_ok = false;
                if (first_error_index < 0) {
                    first_error_index = i;
                    snprintf(first_error, sizeof(first_error), "%s", controller->last_error);
                }
            }
        }

        if (!newly_opened && !corsair_refresh(&controller->device, err, sizeof(err))) {
            snprintf(controller->last_error, sizeof(controller->last_error), "%s", err);
            if (first_error_index < 0) {
                first_error_index = i;
                snprintf(first_error, sizeof(first_error), "%s", err);
            }
            close_controller(app, i);
            continue;
        }

        if (!apply_nvidia_curve_to_controller(app, i, err, sizeof(err))) {
            snprintf(controller->last_error, sizeof(controller->last_error), "%s", err);
            controller_ok = false;
            if (first_error_index < 0) {
                first_error_index = i;
                snprintf(first_error, sizeof(first_error), "%s", err);
            }
        }

        if (controller_ok) {
            controller->last_error[0] = '\0';
        }
    }

    update_controls_enabled(app);
    update_status_view(app);
    if (first_error_index >= 0) {
        char context[64];
        snprintf(context, sizeof(context), "Controller %d update", first_error_index + 1);
        set_status_from_error(app, context, first_error);
    }
}

static bool apply_one(AppState *app, int device_index, int fan, char *err, size_t err_len)
{
    if (device_index < 0 || device_index >= app->device_count) {
        snprintf(err, err_len, "Invalid controller index.");
        return false;
    }

    ControllerRuntime *controller = &app->controllers[device_index];
    if (!controller->opened) {
        snprintf(err, err_len, "Controller is not initialized.");
        return false;
    }

    CorsairFanMode mode = configured_mode(controller, fan);
    int duty = controller->fan[fan].duty_percent;

    if (fan_uses_nvidia_curve(controller, fan)) {
        if (!app->gpu_ok) {
            snprintf(err, err_len, "NVIDIA temperature is not available.");
            return false;
        }
        if (controller->device.status.fan_mode[fan] != CORSAIR_FAN_PWM &&
            controller->device.status.fan_mode[fan] != CORSAIR_FAN_DC) {
            snprintf(err, err_len, "Fan %d is not connected as PWM or DC.", fan + 1);
            return false;
        }
        duty = nvidia_curve_duty(app);
        if (!corsair_set_fan_duty(&controller->device, fan, duty, err, err_len)) {
            return false;
        }
        controller->last_nvidia_duty[fan] = duty;
        return true;
    }

    controller->last_nvidia_duty[fan] = -1;
    if ((int)mode != controller->device.status.fan_mode[fan]) {
        if (!corsair_set_fan_mode(&controller->device, fan, mode, err, err_len)) {
            return false;
        }
    }

    if (mode == CORSAIR_FAN_DISCONNECTED) {
        return true;
    }

    return corsair_set_fan_duty(&controller->device, fan, duty, err, err_len);
}

static void apply_fan(AppState *app, int fan)
{
    char err[256] = { 0 };
    int device_index = selected_device_index(app);
    if (device_index < 0 || !app->controllers[device_index].opened) {
        set_status(app, L"Select an initialized device first.");
        return;
    }

    capture_device_settings(app, device_index);
    if (!apply_one(app, device_index, fan, err, sizeof(err))) {
        set_status_from_error(app, "Apply", err);
        return;
    }
    app->controllers[device_index].fan[fan].apply = true;
    save_settings(app);
    refresh_status(app);
}

static void apply_all(AppState *app)
{
    char err[256] = { 0 };
    int device_index = selected_device_index(app);
    if (device_index < 0 || !app->controllers[device_index].opened) {
        set_status(app, L"Select an initialized device first.");
        return;
    }

    capture_device_settings(app, device_index);
    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        if (!apply_one(app, device_index, i, err, sizeof(err))) {
            set_status_from_error(app, "Apply all", err);
            return;
        }
        app->controllers[device_index].fan[i].apply = true;
    }
    save_settings(app);
    refresh_status(app);
}

static HWND make_child(HWND parent, const wchar_t *class_name, const wchar_t *text,
                       DWORD style, int x, int y, int w, int h, int id)
{
    return CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, parent, (HMENU)(INT_PTR)id,
                           GetModuleHandleW(NULL), NULL);
}

static void create_controls(AppState *app, HWND hwnd)
{
    app->device_combo = make_child(hwnd, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                                   12, 12, 320, 160, IDC_DEVICE_COMBO);
    app->scan_btn = make_child(hwnd, WC_BUTTONW, L"Scan", BS_PUSHBUTTON,
                               344, 12, 80, 26, IDC_SCAN);
    app->refresh_btn = make_child(hwnd, WC_BUTTONW, L"Refresh", BS_PUSHBUTTON,
                                  432, 12, 88, 26, IDC_REFRESH);
    app->apply_all_btn = make_child(hwnd, WC_BUTTONW, L"Apply all", BS_PUSHBUTTON,
                                    528, 12, 90, 26, IDC_APPLY_ALL);
    app->autostart_checkbox = make_child(hwnd, WC_BUTTONW, L"Autostart", BS_AUTOCHECKBOX,
                                         632, 44, 92, 22, IDC_AUTOSTART);
    app->save_btn = make_child(hwnd, WC_BUTTONW, L"Save", BS_PUSHBUTTON,
                               536, 44, 88, 22, IDC_SAVE_SETTINGS);
    SendMessageW(app->autostart_checkbox, BM_SETCHECK,
                 is_autostart_enabled() ? BST_CHECKED : BST_UNCHECKED, 0);

    make_child(hwnd, WC_STATICW, L"Fan", 0, 18, 54, 40, 20, 0);
    make_child(hwnd, WC_STATICW, L"Mode", 0, 82, 54, 70, 20, 0);
    make_child(hwnd, WC_STATICW, L"Status", 0, 168, 54, 120, 20, 0);
    make_child(hwnd, WC_STATICW, L"Duty", 0, 372, 54, 60, 20, 0);

    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        wchar_t label[32];
        int y = 80 + i * 38;

        swprintf(label, sizeof(label) / sizeof(label[0]), L"Fan %d", i + 1);
        make_child(hwnd, WC_STATICW, label, 0, 18, y + 4, 52, 20, 0);

        app->fan_mode[i] = make_child(hwnd, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                                      78, y, 76, 100, IDC_MODE_BASE + i);
        SendMessageW(app->fan_mode[i], CB_ADDSTRING, 0, (LPARAM)L"Detected");
        SendMessageW(app->fan_mode[i], CB_ADDSTRING, 0, (LPARAM)L"PWM");
        SendMessageW(app->fan_mode[i], CB_ADDSTRING, 0, (LPARAM)L"DC");
        SendMessageW(app->fan_mode[i], CB_ADDSTRING, 0, (LPARAM)L"Off");
        SendMessageW(app->fan_mode[i], CB_ADDSTRING, 0, (LPARAM)L"NVIDIA");
        SendMessageW(app->fan_mode[i], CB_SETCURSEL, 0, 0);

        app->fan_rpm[i] = make_child(hwnd, WC_STATICW, L"N/A / 0 rpm", 0,
                                     168, y + 4, 120, 20, IDC_RPM_BASE + i);

        app->fan_slider[i] = make_child(hwnd, TRACKBAR_CLASSW, L"", TBS_AUTOTICKS,
                                        294, y - 2, 180, 30, IDC_DUTY_BASE + i);
        SendMessageW(app->fan_slider[i], TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(app->fan_slider[i], TBM_SETTICFREQ, 10, 0);
        SendMessageW(app->fan_slider[i], TBM_SETPOS, TRUE, 50);

        app->fan_duty[i] = make_child(hwnd, WC_STATICW, L"50%", 0,
                                      486, y + 4, 44, 20, IDC_DUTY_LABEL_BASE + i);
        app->fan_apply[i] = make_child(hwnd, WC_BUTTONW, L"Apply", BS_PUSHBUTTON,
                                       540, y, 82, 26, IDC_APPLY_BASE + i);
    }

    make_child(hwnd, WC_STATICW, L"Temperature probes", 0, 18, 322, 140, 20, 0);
    for (int i = 0; i < CORSAIR_TEMP_COUNT; ++i) {
        app->temp_label[i] = make_child(hwnd, WC_STATICW, L"T: N/A", 0,
                                        18 + i * 116, 348, 100, 20, IDC_TEMP_BASE + i);
    }

    make_child(hwnd, WC_STATICW, L"SATA rails", 0, 18, 382, 140, 20, 0);
    for (int i = 0; i < CORSAIR_VOLT_COUNT; ++i) {
        app->volt_label[i] = make_child(hwnd, WC_STATICW, L"V: N/A", 0,
                                        18 + i * 116, 408, 100, 20, IDC_VOLT_BASE + i);
    }

    make_child(hwnd, WC_STATICW, L"NVIDIA curve", 0, 500, 322, 100, 20, 0);
    app->gpu_status_label = make_child(hwnd, WC_STATICW, L"GPU temp: N/A", 0,
                                       500, 348, 220, 20, IDC_GPU_STATUS);
    make_child(hwnd, WC_STATICW, L"Low temp/%", 0, 500, 382, 86, 20, 0);
    app->gpu_temp_low_edit = make_child(hwnd, WC_EDITW, L"40", WS_BORDER | ES_NUMBER,
                                        590, 380, 38, 24, IDC_GPU_TEMP_LOW);
    app->gpu_duty_low_edit = make_child(hwnd, WC_EDITW, L"25", WS_BORDER | ES_NUMBER,
                                        634, 380, 38, 24, IDC_GPU_DUTY_LOW);
    make_child(hwnd, WC_STATICW, L"High temp/%", 0, 500, 414, 86, 20, 0);
    app->gpu_temp_high_edit = make_child(hwnd, WC_EDITW, L"80", WS_BORDER | ES_NUMBER,
                                         590, 412, 38, 24, IDC_GPU_TEMP_HIGH);
    app->gpu_duty_high_edit = make_child(hwnd, WC_EDITW, L"100", WS_BORDER | ES_NUMBER,
                                         634, 412, 38, 24, IDC_GPU_DUTY_HIGH);

    app->status_label = make_child(hwnd, WC_STATICW, L"Scan for devices.", SS_LEFT,
                                   12, 448, 710, 42, IDC_STATUS);

    load_global_settings(app);
    refresh_gpu_status(app);
    update_controls_enabled(app);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    AppState *app = &g_app;

    if (app->taskbar_created_msg != 0 && msg == app->taskbar_created_msg) {
        app->tray_added = false;
        if (app->tray_icon_wanted) {
            ensure_tray_icon(app);
        }
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        app->hwnd = hwnd;
        create_controls(app, hwnd);
        scan_devices(app);
        SetTimer(hwnd, REFRESH_TIMER, POLL_INTERVAL_MS, NULL);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wparam);
        if (id == IDC_DEVICE_COMBO && HIWORD(wparam) == CBN_SELCHANGE) {
            int index = selected_device_index(app);
            if (index >= 0) {
                activate_device_selection(app, index, true);
                open_selected_device(app);
            }
        } else if (id == IDC_SCAN) {
            scan_devices(app);
        } else if (id == IDC_REFRESH) {
            refresh_status(app);
        } else if (id == IDC_APPLY_ALL) {
            apply_all(app);
        } else if (id >= IDC_APPLY_BASE && id < IDC_APPLY_BASE + CORSAIR_FAN_COUNT) {
            apply_fan(app, id - IDC_APPLY_BASE);
        } else if (id == IDC_SAVE_SETTINGS) {
            int device_index = selected_device_index(app);
            if (device_index >= 0) {
                capture_device_settings(app, device_index);
                for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
                    app->controllers[device_index].fan[i].apply = true;
                }
            }
            save_settings(app);

            bool apply_ok = true;
            char first_error[256] = { 0 };
            for (int i = 0; i < app->device_count; ++i) {
                if (!app->controllers[i].opened) {
                    continue;
                }
                char err[256] = { 0 };
                if (!apply_saved_fan_settings(app, i, err, sizeof(err))) {
                    apply_ok = false;
                    if (!first_error[0]) {
                        snprintf(first_error, sizeof(first_error), "Controller %d: %s", i + 1, err);
                    }
                }
            }

            refresh_status(app);
            for (int i = 0; i < app->device_count; ++i) {
                if (!app->controllers[i].opened || app->controllers[i].last_error[0]) {
                    apply_ok = false;
                    if (!first_error[0]) {
                        snprintf(first_error, sizeof(first_error), "Controller %d: %s", i + 1,
                                 app->controllers[i].last_error[0]
                                     ? app->controllers[i].last_error
                                     : "could not be opened");
                    }
                }
            }
            if (!apply_ok) {
                set_status_from_error(app, "Save/apply", first_error);
            } else {
                set_status(app, L"Settings saved; all controllers updated.");
            }
        } else if (id == IDC_AUTOSTART && HIWORD(wparam) == BN_CLICKED) {
            bool enabled = SendMessageW(app->autostart_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!set_autostart_enabled(enabled)) {
                SendMessageW(app->autostart_checkbox, BM_SETCHECK, enabled ? BST_UNCHECKED : BST_CHECKED, 0);
                set_status(app, L"Autostart registry update failed.");
            } else {
                save_settings(app);
                set_status(app, enabled ? L"Autostart enabled." : L"Autostart disabled.");
            }
        } else if (id == IDM_TRAY_OPEN) {
            show_from_tray(app);
        } else if (id == IDM_TRAY_EXIT) {
            app->allow_close = true;
            DestroyWindow(hwnd);
        } else if (id >= IDC_MODE_BASE && id < IDC_MODE_BASE + CORSAIR_FAN_COUNT &&
                   HIWORD(wparam) == CBN_SELCHANGE) {
            int device_index = selected_device_index(app);
            if (device_index >= 0) {
                capture_device_settings(app, device_index);
                app->controllers[device_index].fan[id - IDC_MODE_BASE].apply = true;
                save_settings(app);
            }
        } else if ((id == IDC_GPU_TEMP_LOW || id == IDC_GPU_DUTY_LOW ||
                    id == IDC_GPU_TEMP_HIGH || id == IDC_GPU_DUTY_HIGH) &&
                   HIWORD(wparam) == EN_KILLFOCUS) {
            save_settings(app);
        }
        return 0;
    }

    case WM_HSCROLL:
        for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
            if ((HWND)lparam == app->fan_slider[i]) {
                update_duty_label(app, i);
                if (LOWORD(wparam) == TB_ENDTRACK || LOWORD(wparam) == SB_ENDSCROLL) {
                    int device_index = selected_device_index(app);
                    if (device_index >= 0) {
                        capture_device_settings(app, device_index);
                        app->controllers[device_index].fan[i].apply = true;
                        save_settings(app);
                    }
                }
                break;
            }
        }
        return 0;

    case WM_TIMER:
        if (wparam == REFRESH_TIMER) {
            refresh_status(app);
        } else if (wparam == TRAY_RETRY_TIMER) {
            if (app->tray_icon_wanted && !app->tray_added) {
                ensure_tray_icon(app);
            } else {
                KillTimer(hwnd, TRAY_RETRY_TIMER);
            }
        }
        return 0;

    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) {
            save_settings(app);
            hide_to_tray(app);
            return 0;
        }
        break;

    case WM_TRAYICON:
        if (lparam == WM_LBUTTONDBLCLK || lparam == WM_LBUTTONUP) {
            show_from_tray(app);
        } else if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
            show_tray_menu(app);
        }
        return 0;

    case WM_CLOSE:
        if (!app->allow_close) {
            save_settings(app);
            hide_to_tray(app);
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, REFRESH_TIMER);
        KillTimer(hwnd, TRAY_RETRY_TIMER);
        save_settings(app);
        if (app->tray_added) {
            tray_update(app, NIM_DELETE);
        }
        app->tray_icon_wanted = false;
        close_all_controllers(app);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev_instance, LPSTR cmd_line, int show_cmd)
{
    (void)prev_instance;

    INITCOMMONCONTROLSEX icc;
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;

    ZeroMemory(&g_app, sizeof(g_app));
    g_app.active_device_index = -1;
    g_app.taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");
    g_app.start_in_tray = strstr(cmd_line, "--tray") != NULL;

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"CorsairNvidiaFanControlWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"Could not register window class.", APP_TITLE, MB_ICONERROR);
        return 1;
    }

    hwnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE,
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, 750, 540,
                           NULL, NULL, instance, NULL);
    if (!hwnd) {
        MessageBoxW(NULL, L"Could not create main window.", APP_TITLE, MB_ICONERROR);
        return 1;
    }

    if (g_app.start_in_tray) {
        hide_to_tray(&g_app);
    } else {
        ShowWindow(hwnd, show_cmd);
        UpdateWindow(hwnd);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
