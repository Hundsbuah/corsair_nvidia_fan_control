#include "corsair_hid.h"
#include "nvidia_temp.h"
#include "resource.h"
#include "ui.h"

#include <commctrl.h>
#include <math.h>
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
#define IDC_OVERVIEW 102
#define IDC_REFRESH 103
#define IDC_APPLY_ALL 104
#define IDC_STATUS 105
#define IDC_DUTY_BASE 200
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

#define FAKE_DEVICE_ENV "CFC_FAKE"
#define FAKE_PATH_PREFIX L"fake://"

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
    /* Header */
    HWND title_label;
    HWND device_subtitle;
    HWND gpu_caption;
    HWND gpu_readout;
    HWND status_dot;
    /* Left column */
    HWND fans_panel;
    HWND sensors_panel;
    HWND rails_panel;
    HWND chip_panels_temp[CORSAIR_TEMP_COUNT];
    HWND chip_panels_volt[CORSAIR_VOLT_COUNT];
    /* Right column */
    HWND curve_panel;
    HWND curve_graph;
    HWND gpu_status_label;
    HWND controller_panel;
    HWND ctrl_state_label;
    HWND ctrl_fw_label;
    HWND ctrl_bl_label;
    HWND ctrl_err_label;
    HWND options_panel;
    /* Fan rows */
    HWND fan_labels[CORSAIR_FAN_COUNT];
    HWND fan_mode[CORSAIR_FAN_COUNT];
    HWND fan_rpm[CORSAIR_FAN_COUNT];
    HWND fan_slider[CORSAIR_FAN_COUNT];
    HWND fan_duty[CORSAIR_FAN_COUNT];
    HWND fan_apply[CORSAIR_FAN_COUNT];
    /* Sensor / rail values */
    HWND temp_label[CORSAIR_TEMP_COUNT];
    HWND volt_label[CORSAIR_VOLT_COUNT];
    /* Footer */
    HWND device_combo;
    HWND scan_btn;
    HWND refresh_btn;
    HWND apply_all_btn;
    HWND overview_btn;
    HWND autostart_checkbox;
    HWND save_btn;
    HWND status_label;
    /* GPU curve edits */
    HWND gpu_temp_low_edit;
    HWND gpu_duty_low_edit;
    HWND gpu_temp_high_edit;
    HWND gpu_duty_high_edit;
    /* Panel headings / static labels (moved by layout) */
    HWND fans_heading;
    HWND sensors_heading;
    HWND rails_heading;
    HWND curve_heading;
    HWND low_high_labels[2];
    HWND pct_labels[2];
    HWND controller_heading;
    HWND ctrl_row_labels[4];
    HWND options_heading;
    HWND chip_labels_temp[CORSAIR_TEMP_COUNT];
    HWND chip_labels_volt[CORSAIR_VOLT_COUNT];
    /* Overview window */
    HWND overview_window;
    HWND overview_gpu_label;
    HWND overview_fans_panel;
    HWND overview_fan_heading;
    HWND overview_fan_list;
    HWND overview_sensor_panel;
    HWND overview_sensor_heading;
    HWND overview_sensor_list;
    /* Device / controller state */
    CorsairDeviceInfo devices[CORSAIR_MAX_DEVICES];
    int device_count;
    int saved_device_index;
    int active_device_index;
    wchar_t last_device_key[32];
    ControllerRuntime controllers[CORSAIR_MAX_DEVICES];
    NvidiaGpuStatus gpu;
    bool gpu_ok;
    SYSTEMTIME last_poll_time;
    bool has_poll_time;
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
static void update_duty_label(AppState *app, int fan);
static void show_device_settings(AppState *app, int device_index);
static void clear_device_status_view(AppState *app);
static void update_status_view(AppState *app);
static void refresh_gpu_status(AppState *app);
static CorsairFanMode configured_mode(ControllerRuntime *controller, int fan);
static bool fan_uses_nvidia_curve(const ControllerRuntime *controller, int fan);
static bool apply_one(AppState *app, int device_index, int fan, char *err,
                      size_t err_len);
static bool apply_saved_fan_settings(AppState *app, int device_index, char *err,
                                     size_t err_len);
static bool apply_nvidia_curve_to_controller(AppState *app, int device_index,
                                             char *err, size_t err_len);
static bool open_controller(AppState *app, int device_index, char *err,
                            size_t err_len);
static void scan_devices(AppState *app);
static void open_selected_device(AppState *app);
static void refresh_status(AppState *app);
static void apply_fan(AppState *app, int fan);
static void apply_all(AppState *app);
static void layout_main(AppState *app);
static void layout_overview_controls(AppState *app, int width, int height);
static void create_main_controls(AppState *app, HWND hwnd);
static void create_overview_controls(AppState *app, HWND hwnd);
static void update_overview_view(AppState *app);
static void update_visual_state(AppState *app);
static void update_curve_points(AppState *app);
static void update_device_subtitle(AppState *app);
static const wchar_t *fan_profile_name(int mode_index);
static void show_overview_window(AppState *app);
static void set_status(AppState *app, const wchar_t *text);
static void set_status_from_error(AppState *app, const char *prefix, const char *err);
static LRESULT CALLBACK overview_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                          LPARAM lparam);
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

/* ================================================================ helpers */

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
    return RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, access, NULL,
                           key, NULL) == ERROR_SUCCESS;
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

static bool settings_read_string(HKEY key, const wchar_t *name, wchar_t *value,
                                 DWORD value_count)
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

static void device_settings_key(const CorsairDeviceInfo *info, wchar_t *key,
                                size_t key_count)
{
    uint64_t hash = 1469598103934665603ULL;
    const wchar_t *text = info && info->path[0] ? info->path : L"unknown";

    for (size_t i = 0; text[i] != L'\0'; ++i) {
        hash ^= (uint64_t)(uint16_t)text[i];
        hash *= 1099511628211ULL;
    }

    swprintf(key, key_count, L"%016llX", (unsigned long long)hash);
}

static bool settings_open_device_key_for_index(AppState *app, int device_index,
                                               HKEY *key, REGSAM access)
{
    if (device_index < 0 || device_index >= app->device_count) {
        return false;
    }

    wchar_t device_key[32];
    wchar_t path[160];
    device_settings_key(&app->devices[device_index], device_key,
                        sizeof(device_key) / sizeof(device_key[0]));
    swprintf(path, sizeof(path) / sizeof(path[0]), SETTINGS_KEY L"\\Devices\\%s",
             device_key);

    if (access & (KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE | WRITE_DAC |
                  WRITE_OWNER)) {
        return settings_create_key(path, key, access);
    }
    return settings_open_existing_key(path, key, access);
}

static bool settings_open_legacy_device_key_for_index(AppState *app, int device_index,
                                                      HKEY *key, REGSAM access)
{
    if (device_index < 0 || device_index >= app->device_count) {
        return false;
    }

    wchar_t device_key[32];
    wchar_t path[160];
    device_settings_key(&app->devices[device_index], device_key,
                        sizeof(device_key) / sizeof(device_key[0]));
    swprintf(path, sizeof(path) / sizeof(path[0]), LEGACY_SETTINGS_KEY L"\\Devices\\%s",
             device_key);
    return settings_open_existing_key(path, key, access);
}

static int find_device_by_key(AppState *app, const wchar_t *key)
{
    if (!key || key[0] == L'\0') {
        return -1;
    }

    for (int i = 0; i < app->device_count; ++i) {
        wchar_t current[32];
        device_settings_key(&app->devices[i], current,
                            sizeof(current) / sizeof(current[0]));
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

/* ========================================================= fake device */

static int g_fake_duty[CORSAIR_FAN_COUNT];

static bool fake_device_enabled(void)
{
    return getenv(FAKE_DEVICE_ENV) != NULL;
}

static bool is_fake_device(const CorsairDevice *dev)
{
    return wcsncmp(dev->info.path, FAKE_PATH_PREFIX,
                   wcslen(FAKE_PATH_PREFIX)) == 0;
}

static void fake_fill_base(CorsairStatus *status)
{
    static const int base_mode[CORSAIR_FAN_COUNT] = {
        CORSAIR_FAN_PWM, CORSAIR_FAN_PWM, CORSAIR_FAN_DISCONNECTED,
        CORSAIR_FAN_PWM, CORSAIR_FAN_DISCONNECTED, CORSAIR_FAN_PWM
    };
    static const int base_rpm[CORSAIR_FAN_COUNT] = {
        2480, 2960, 0, 3120, 0, 1960
    };

    status->firmware[0] = 1;
    status->firmware[1] = 2;
    status->firmware[2] = 3;
    status->bootloader[0] = 2;
    status->bootloader[1] = 1;
    status->temp_connected[0] = 1;
    status->temp_connected[1] = 1;
    status->temp_connected[2] = 0;
    status->temp_connected[3] = 1;
    status->temp_c[0] = 27.5;
    status->temp_c[1] = 31.2;
    status->temp_c[2] = 0.0;
    status->temp_c[3] = 44.8;
    status->volts[0] = 12.04;
    status->volts[1] = 5.01;
    status->volts[2] = 3.29;
    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        status->fan_mode[i] = base_mode[i];
        g_fake_duty[i] = 50;
        status->fan_rpm[i] =
            base_mode[i] == CORSAIR_FAN_DISCONNECTED ? 0 : base_rpm[i] * 50 / 100;
    }
}

static void fake_refresh(AppState *app, int device_index)
{
    static const int base_rpm[CORSAIR_FAN_COUNT] = {
        2480, 2960, 0, 3120, 0, 1960
    };
    CorsairStatus *status = &app->controllers[device_index].device.status;
    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        if (status->fan_mode[i] == CORSAIR_FAN_DISCONNECTED || base_rpm[i] == 0) {
            status->fan_rpm[i] = 0;
            continue;
        }
        int target = base_rpm[i] * g_fake_duty[i] / 100;
        int jitter = (rand() % 21) - 10;
        int rpm = target + jitter;
        if (rpm < 0) {
            rpm = 0;
        }
        status->fan_rpm[i] = rpm;
    }
    status->temp_c[0] += ((rand() % 5) - 2) * 0.1;
    status->temp_c[3] += ((rand() % 5) - 2) * 0.1;
}

static void fake_set_duty(AppState *app, int device_index, int fan, int duty)
{
    static const int base_rpm[CORSAIR_FAN_COUNT] = {
        2480, 2960, 0, 3120, 0, 1960
    };
    g_fake_duty[fan] = duty;
    if (app->controllers[device_index].device.status.fan_mode[fan] !=
        CORSAIR_FAN_DISCONNECTED &&
        base_rpm[fan] != 0) {
        app->controllers[device_index].device.status.fan_rpm[fan] =
            base_rpm[fan] * duty / 100;
    }
}

/* =========================================================== device logic */

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
        ui_combo_set_selected(app->fan_mode[i], controller->fan[i].mode_index);
        ui_slider_set(app->fan_slider[i], controller->fan[i].duty_percent);
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
        int mode = ui_combo_selected(app->fan_mode[i]);
        controller->fan[i].mode_index =
            mode < 0 ? FAN_MODE_DETECTED_INDEX
                     : clamp_int(mode, 0, FAN_MODE_NVIDIA_INDEX);
        controller->fan[i].duty_percent =
            clamp_int(ui_slider_value(app->fan_slider[i]), 0, 100);
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
                mode == FAN_MODE_NVIDIA_INDEX || mode == FAN_MODE_OFF_INDEX ||
                duty_exists;
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
                         (DWORD)(sizeof(app->last_device_key) /
                                 sizeof(app->last_device_key[0])));
    set_edit_int(app->gpu_temp_low_edit,
                 (int)settings_read_dword(key, L"GpuTempLow", 40));
    set_edit_int(app->gpu_duty_low_edit,
                 (int)settings_read_dword(key, L"GpuDutyLow", 25));
    set_edit_int(app->gpu_temp_high_edit,
                 (int)settings_read_dword(key, L"GpuTempHigh", 80));
    set_edit_int(app->gpu_duty_high_edit,
                 (int)settings_read_dword(key, L"GpuDutyHigh", 100));

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
    if (!loaded &&
        settings_open_legacy_device_key_for_index(app, device_index, &key,
                                                  KEY_QUERY_VALUE)) {
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
        copy_wstr(app->last_device_key,
                  sizeof(app->last_device_key) / sizeof(app->last_device_key[0]),
                  device_key);
        app->saved_device_index = device_index;
    }

    settings_write_dword(key, L"GpuTempLow",
                         (DWORD)clamp_int(edit_int(app->gpu_temp_low_edit, 40), 0, 120));
    settings_write_dword(key, L"GpuDutyLow",
                         (DWORD)clamp_int(edit_int(app->gpu_duty_low_edit, 25), 0, 100));
    settings_write_dword(key, L"GpuTempHigh",
                         (DWORD)clamp_int(edit_int(app->gpu_temp_high_edit, 80), 0, 120));
    settings_write_dword(key, L"GpuDutyHigh",
                         (DWORD)clamp_int(edit_int(app->gpu_duty_high_edit, 100), 0, 100));

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

/* ================================================================== tray */

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
    copy_wstr(nid.szTip, sizeof(nid.szTip) / sizeof(nid.szTip[0]),
              L"Corsair NVIDIA Fan Control");

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
    if (app->overview_window && IsWindow(app->overview_window)) {
        ShowWindow(app->overview_window, SW_HIDE);
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

/* ============================================================ main window */

static HWND make_child(HWND parent, const wchar_t *class_name, const wchar_t *text,
                       DWORD style, int x, int y, int w, int h, int id)
{
    return CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, parent, (HMENU)(INT_PTR)id,
                           GetModuleHandleW(NULL), NULL);
}

static void set_status(AppState *app, const wchar_t *text)
{
    ui_register_ctrl(app->status_label, UI_BG_INK, ui_theme()->dim);
    SetWindowTextW(app->status_label, text);
    InvalidateRect(app->status_label, NULL, FALSE);
}

static void set_status_from_error(AppState *app, const char *prefix, const char *err)
{
    wchar_t wbuf[512];
    char buf[512];
    snprintf(buf, sizeof(buf), "%s: %s", prefix, err && err[0] ? err : "Unknown error");
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf,
                        (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    ui_register_ctrl(app->status_label, UI_BG_INK, ui_theme()->warn);
    SetWindowTextW(app->status_label, wbuf);
    InvalidateRect(app->status_label, NULL, FALSE);
}

static int selected_device_index(AppState *app)
{
    int index = ui_combo_selected(app->device_combo);
    if (index < 0 || index >= app->device_count) {
        return -1;
    }
    return index;
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
    if (app->active_device_index >= 0 &&
        app->active_device_index < app->device_count) {
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
    swprintf(text, sizeof(text) / sizeof(text[0]), L"%d%%",
             ui_slider_value(app->fan_slider[fan]));
    SetWindowTextW(app->fan_duty[fan], text);
}

static void update_gpu_status_view(AppState *app, const char *err)
{
    wchar_t text[256];

    if (app->gpu_ok) {
        wchar_t gpu_name[96];
        MultiByteToWideChar(CP_UTF8, 0, app->gpu.name, -1, gpu_name,
                            (int)(sizeof(gpu_name) / sizeof(gpu_name[0])));
        swprintf(text, sizeof(text) / sizeof(text[0]), L"%s · %d °C",
                 gpu_name[0] ? gpu_name : L"NVIDIA GPU", app->gpu.temperature_c);
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
    if (!app->gpu_ok && fake_device_enabled()) {
        app->gpu.available = true;
        app->gpu.gpu_count = 1;
        strcpy(app->gpu.name, "GeForce RTX 4090 (Test)");
        double phase = (double)GetTickCount() / 25000.0;
        app->gpu.temperature_c = (int)(56.0 + 10.0 * sin(phase));
        app->gpu_ok = true;
    }
    update_gpu_status_view(app, app->gpu_ok ? NULL : err);
    ui_curve_set_now(app->curve_graph, app->gpu.temperature_c, app->gpu_ok);

    wchar_t readout[32];
    if (app->gpu_ok) {
        swprintf(readout, sizeof(readout) / sizeof(readout[0]), L"%d °C",
                 app->gpu.temperature_c);
    } else {
        copy_wstr(readout, sizeof(readout) / sizeof(readout[0]), L"— °C");
    }
    SetWindowTextW(app->gpu_readout, readout);
}

static void update_curve_points(AppState *app)
{
    ui_curve_set_points(app->curve_graph,
                        clamp_int(edit_int(app->gpu_temp_low_edit, 40), 0, 120),
                        clamp_int(edit_int(app->gpu_duty_low_edit, 25), 0, 100),
                        clamp_int(edit_int(app->gpu_temp_high_edit, 80), 0, 120),
                        clamp_int(edit_int(app->gpu_duty_high_edit, 100), 0, 100));
}

static void update_visual_state(AppState *app)
{
    int opened = 0;
    int errors = 0;
    for (int i = 0; i < app->device_count; ++i) {
        ControllerRuntime *controller = &app->controllers[i];
        if (!controller->opened) {
            continue;
        }
        ++opened;
        if (controller->last_error[0]) {
            ++errors;
        }
    }

    ui_dot_set(app->status_dot,
               opened == 0 ? UI_DOT_OFF
                           : (errors > 0 ? UI_DOT_WARN : UI_DOT_OK));
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
                 L"%s · %d rpm",
                 corsair_fan_mode_name(controller->device.status.fan_mode[i]),
                 controller->device.status.fan_rpm[i]);
        SetWindowTextW(app->fan_rpm[i], text);
    }

    for (int i = 0; i < CORSAIR_TEMP_COUNT; ++i) {
        if (controller->device.status.temp_connected[i]) {
            swprintf(text, sizeof(text) / sizeof(text[0]), L"%.1f °C",
                     controller->device.status.temp_c[i]);
        } else {
            copy_wstr(text, sizeof(text) / sizeof(text[0]), L"N/A");
        }
        SetWindowTextW(app->temp_label[i], text);
    }

    for (int i = 0; i < CORSAIR_VOLT_COUNT; ++i) {
        swprintf(text, sizeof(text) / sizeof(text[0]), L"%.2f V",
                 controller->device.status.volts[i]);
        SetWindowTextW(app->volt_label[i], text);
    }

    swprintf(text, sizeof(text) / sizeof(text[0]), L"%d.%d.%d",
             controller->device.status.firmware[0],
             controller->device.status.firmware[1],
             controller->device.status.firmware[2]);
    SetWindowTextW(app->ctrl_fw_label, text);
    swprintf(text, sizeof(text) / sizeof(text[0]), L"%d.%d",
             controller->device.status.bootloader[0],
             controller->device.status.bootloader[1]);
    SetWindowTextW(app->ctrl_bl_label, text);

    if (controller->last_error[0]) {
        wchar_t werr[256];
        MultiByteToWideChar(CP_UTF8, 0, controller->last_error, -1, werr,
                            (int)(sizeof(werr) / sizeof(werr[0])));
        SetWindowTextW(app->ctrl_err_label, werr);
        SetWindowTextW(app->ctrl_state_label, L"Degraded");
        set_status_from_error(app, "Controller", controller->last_error);
    } else {
        SetWindowTextW(app->ctrl_err_label, L"");
        SetWindowTextW(app->ctrl_state_label, L"Online");
    }
}

static void clear_device_status_view(AppState *app)
{
    wchar_t text[64];
    (void)text;

    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        SetWindowTextW(app->fan_rpm[i], L"· · · rpm");
    }
    for (int i = 0; i < CORSAIR_TEMP_COUNT; ++i) {
        SetWindowTextW(app->temp_label[i], L"N/A");
    }
    for (int i = 0; i < CORSAIR_VOLT_COUNT; ++i) {
        SetWindowTextW(app->volt_label[i], L"N/A");
    }
    SetWindowTextW(app->ctrl_fw_label, L"—");
    SetWindowTextW(app->ctrl_bl_label, L"—");
    SetWindowTextW(app->ctrl_state_label, L"Offline");
    SetWindowTextW(app->ctrl_err_label, L"");
}

static void update_device_subtitle(AppState *app)
{
    int device_index = selected_device_index(app);
    wchar_t text[160];
    if (device_index < 0) {
        copy_wstr(text, sizeof(text) / sizeof(text[0]), L"No devices detected");
    } else {
        swprintf(text, sizeof(text) / sizeof(text[0]), L"%s · PID %04X",
                 app->devices[device_index].model,
                 app->devices[device_index].product_id);
    }
    SetWindowTextW(app->device_subtitle, text);
}

static void close_controller(AppState *app, int device_index)
{
    ControllerRuntime *controller = &app->controllers[device_index];
    if (controller->opened) {
        if (!is_fake_device(&controller->device)) {
            corsair_close(&controller->device);
        }
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

static void activate_device_selection(AppState *app, int device_index,
                                      bool save_previous)
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

    ui_combo_set_selected(app->device_combo, device_index);
    app->active_device_index = device_index;
    show_device_settings(app, device_index);
    clear_device_status_view(app);
    update_controls_enabled(app);
    update_device_subtitle(app);
    save_global_settings(app);
    if (app->controllers[device_index].opened) {
        update_status_view(app);
    } else {
        set_status(app, L"Controller selected; opening it now.");
    }
}

static bool apply_saved_fan_settings(AppState *app, int device_index, char *err,
                                     size_t err_len)
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
        snprintf(err, err_len, "%s",
                 last_err[0] ? last_err
                             : "Some fan settings could not be applied.");
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

    if (is_fake_device(&controller->device)) {
        fake_fill_base(&controller->device.status);
        controller->opened = true;
    } else {
        if (!corsair_open(&controller->device, &app->devices[device_index], err,
                          err_len)) {
            snprintf(controller->last_error, sizeof(controller->last_error), "%s", err);
            return false;
        }

        if (!corsair_initialize(&controller->device, err, err_len)) {
            snprintf(controller->last_error, sizeof(controller->last_error), "%s", err);
            corsair_close(&controller->device);
            return false;
        }
        controller->opened = true;
    }

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

    ui_combo_set_items(app->device_combo, 0, NULL);
    ZeroMemory(app->controllers, sizeof(app->controllers));
    app->device_count = corsair_find_devices(app->devices, CORSAIR_MAX_DEVICES, err,
                                             sizeof(err));

    if (app->device_count == 0 && fake_device_enabled()) {
        CorsairDeviceInfo *dev = &app->devices[0];
        ZeroMemory(dev, sizeof(*dev));
        swprintf(dev->path, sizeof(dev->path) / sizeof(dev->path[0]),
                 L"fake://commander-pro-test");
        lstrcpynW(dev->model, L"Commander Pro (Test)",
                  (int)(sizeof(dev->model) / sizeof(dev->model[0])));
        lstrcpynW(dev->product, L"Commander Pro",
                  (int)(sizeof(dev->product) / sizeof(dev->product[0])));
        dev->vendor_id = 0x1B1C;
        dev->product_id = 0x0C10;
        app->device_count = 1;
        err[0] = '\0';
    }

    app->active_device_index = -1;

    static wchar_t items[CORSAIR_MAX_DEVICES][256];
    const wchar_t *item_ptrs[CORSAIR_MAX_DEVICES];
    for (int i = 0; i < app->device_count; ++i) {
        load_device_settings(app, i);
        swprintf(items[i], sizeof(items[i]) / sizeof(items[i][0]), L"%s (PID %04X)",
                 app->devices[i].model, app->devices[i].product_id);
        item_ptrs[i] = items[i];
    }
    if (app->device_count > 0) {
        ui_combo_set_items(app->device_combo, app->device_count, item_ptrs);
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
                    snprintf(first_error, sizeof(first_error), "Controller %d: %s", i + 1,
                             open_err);
                }
            }
        }

        update_controls_enabled(app);
        update_status_view(app);
        if (error_count > 0) {
            set_status_from_error(app, "Not all controllers were updated", first_error);
        } else {
            swprintf(item, sizeof(item) / sizeof(item[0]),
                     L"%d of %d controllers initialized and updated.", opened_count,
                     app->device_count);
            set_status(app, item);
        }
    } else {
        clear_device_status_view(app);
        update_controls_enabled(app);
        update_device_subtitle(app);
        set_status_from_error(app, "Scan", err);
    }
    GetLocalTime(&app->last_poll_time);
    app->has_poll_time = true;
    update_visual_state(app);
    update_overview_view(app);
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
                    snprintf(first_error, sizeof(first_error), "%s",
                             controller->last_error);
                }
            }
        }

        if (is_fake_device(&controller->device)) {
            fake_refresh(app, i);
        } else if (!newly_opened &&
                   !corsair_refresh(&controller->device, err, sizeof(err))) {
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
        snprintf(context, sizeof(context), "Controller %d update",
                 first_error_index + 1);
        set_status_from_error(app, context, first_error);
    }
    GetLocalTime(&app->last_poll_time);
    app->has_poll_time = true;
    update_visual_state(app);
    update_overview_view(app);
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
    bool fake = is_fake_device(&controller->device);

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
        if (fake) {
            fake_set_duty(app, device_index, fan, duty);
        } else if (!corsair_set_fan_duty(&controller->device, fan, duty, err,
                                         err_len)) {
            return false;
        }
        controller->last_nvidia_duty[fan] = duty;
        return true;
    }

    controller->last_nvidia_duty[fan] = -1;
    if ((int)mode != controller->device.status.fan_mode[fan]) {
        if (fake) {
            controller->device.status.fan_mode[fan] = (int)mode;
        } else if (!corsair_set_fan_mode(&controller->device, fan, mode, err,
                                         err_len)) {
            return false;
        }
    }

    if (mode == CORSAIR_FAN_DISCONNECTED) {
        if (fake) {
            controller->device.status.fan_rpm[fan] = 0;
        }
        return true;
    }

    if (fake) {
        fake_set_duty(app, device_index, fan, duty);
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

/* ==================================================== nvidia curve apply */

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
        bool fake = is_fake_device(&controller->device);
        if (fake) {
            fake_set_duty(app, device_index, i, duty);
        } else if (!corsair_set_fan_duty(&controller->device, i, duty, fan_err,
                                         sizeof(fan_err))) {
            ok = false;
            snprintf(err, err_len, "Fan %d: %s", i + 1, fan_err);
            continue;
        }

        controller->last_nvidia_duty[i] = duty;
        if (device_index == app->active_device_index) {
            ui_slider_set(app->fan_slider[i], duty);
            update_duty_label(app, i);
        }
    }

    return ok;
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
}/* ===================================================== overview window */

static void overview_add_column(HWND list, int index, const wchar_t *title, int width)
{
    LVCOLUMNW column;
    ZeroMemory(&column, sizeof(column));
    column.mask = LVCF_FMT | LVCF_TEXT | LVCF_WIDTH;
    column.fmt = LVCFMT_LEFT;
    column.pszText = (LPWSTR)title;
    column.cx = width;
    ListView_InsertColumn(list, index, &column);
}

static int overview_insert_row(HWND list, const wchar_t *text)
{
    LVITEMW item;
    ZeroMemory(&item, sizeof(item));
    item.mask = LVIF_TEXT;
    item.iItem = ListView_GetItemCount(list);
    item.pszText = (LPWSTR)text;
    return ListView_InsertItem(list, &item);
}

static void overview_set_row_count(HWND list, int row_count)
{
    int current = ListView_GetItemCount(list);
    while (current > row_count) {
        ListView_DeleteItem(list, --current);
    }
    while (current < row_count) {
        overview_insert_row(list, L"");
        ++current;
    }
    if (row_count > 0) {
        ListView_RedrawItems(list, 0, row_count - 1);
    }
}

static void overview_controller_name(AppState *app, int device_index,
                                     wchar_t *text, size_t text_count)
{
    swprintf(text, text_count, L"%d: %s (PID %04X)", device_index + 1,
             app->devices[device_index].model, app->devices[device_index].product_id);
}

static void overview_target_text(AppState *app, int device_index, int fan,
                                 wchar_t *text, size_t text_count)
{
    ControllerRuntime *controller = &app->controllers[device_index];
    FanSettings *settings = &controller->fan[fan];

    if (!settings->apply) {
        copy_wstr(text, text_count, L"-");
    } else if (settings->mode_index == FAN_MODE_OFF_INDEX) {
        copy_wstr(text, text_count, L"Off");
    } else if (settings->mode_index == FAN_MODE_NVIDIA_INDEX) {
        int duty = controller->last_nvidia_duty[fan];
        if (duty < 0 && app->gpu_ok) {
            duty = nvidia_curve_duty(app);
        }
        if (duty >= 0) {
            swprintf(text, text_count, L"%d%%", duty);
        } else {
            copy_wstr(text, text_count, L"N/A");
        }
    } else {
        swprintf(text, text_count, L"%d%%", settings->duty_percent);
    }
}

static void overview_fan_text(void *ctx, int row, int col, wchar_t *text,
                              int text_count)
{
    AppState *app = (AppState *)ctx;
    int device_index = row / CORSAIR_FAN_COUNT;
    int fan = row % CORSAIR_FAN_COUNT;
    if (device_index < 0 || device_index >= app->device_count) {
        text[0] = L'\0';
        return;
    }

    ControllerRuntime *controller = &app->controllers[device_index];
    switch (col) {
    case 0: {
        wchar_t name[256];
        overview_controller_name(app, device_index, name,
                                 sizeof(name) / sizeof(name[0]));
        copy_wstr(text, text_count, name);
        break;
    }
    case 1:
        swprintf(text, text_count, L"Fan %d", fan + 1);
        break;
    case 2:
        copy_wstr(text, text_count, controller->opened
                                       ? corsair_fan_mode_name(
                                             controller->device.status.fan_mode[fan])
                                       : L"Unavailable");
        break;
    case 3:
        if (controller->opened) {
            swprintf(text, text_count, L"%d rpm",
                     controller->device.status.fan_rpm[fan]);
        } else {
            copy_wstr(text, text_count, L"-");
        }
        break;
    case 4:
        copy_wstr(text, text_count,
                  fan_profile_name(controller->fan[fan].mode_index));
        break;
    case 5:
    default: {
        wchar_t target[32];
        overview_target_text(app, device_index, fan, target,
                             sizeof(target) / sizeof(target[0]));
        copy_wstr(text, text_count, target);
        break;
    }
    }
}

static void overview_sensor_text(void *ctx, int row, int col, wchar_t *text,
                                 int text_count)
{
    AppState *app = (AppState *)ctx;
    if (row < 0 || row >= app->device_count) {
        text[0] = L'\0';
        return;
    }

    ControllerRuntime *controller = &app->controllers[row];
    switch (col) {
    case 0: {
        wchar_t name[256];
        overview_controller_name(app, row, name, sizeof(name) / sizeof(name[0]));
        copy_wstr(text, text_count, name);
        break;
    }
    case 1: {
        if (controller->opened) {
            if (controller->last_error[0]) {
                wchar_t state[320];
                wchar_t error[256];
                MultiByteToWideChar(CP_UTF8, 0, controller->last_error, -1, error,
                                    (int)(sizeof(error) / sizeof(error[0])));
                swprintf(state, sizeof(state) / sizeof(state[0]), L"Degraded · %s",
                         error);
                copy_wstr(text, text_count, state);
            } else {
                copy_wstr(text, text_count, L"Online");
            }
        } else {
            wchar_t error[256];
            MultiByteToWideChar(CP_UTF8, 0, controller->last_error, -1, error,
                                (int)(sizeof(error) / sizeof(error[0])));
            wchar_t state[320];
            swprintf(state, sizeof(state) / sizeof(state[0]), L"Offline%s%s",
                     error[0] ? L" · " : L"", error);
            copy_wstr(text, text_count, state);
        }
        break;
    }
    case 2:
    case 3: {
        int *values;
        if (col == 2) {
            values = controller->device.status.firmware;
            swprintf(text, text_count, L"%d.%d.%d", values[0], values[1], values[2]);
        } else {
            values = controller->device.status.bootloader;
            swprintf(text, text_count, L"%d.%d", values[0], values[1]);
        }
        if (!controller->opened) {
            copy_wstr(text, text_count, L"-");
        }
        break;
    }
    case 4:
    case 5:
    case 6:
    case 7: {
        int sensor = col - 4;
        if (controller->opened &&
            controller->device.status.temp_connected[sensor]) {
            swprintf(text, text_count, L"%.1f °C",
                     controller->device.status.temp_c[sensor]);
        } else {
            copy_wstr(text, text_count, L"N/A");
        }
        break;
    }
    default: {
        int rail = col - 8;
        if (rail >= 0 && rail < CORSAIR_VOLT_COUNT && controller->opened) {
            swprintf(text, text_count, L"%.2f V",
                     controller->device.status.volts[rail]);
        } else {
            copy_wstr(text, text_count, L"N/A");
        }
        break;
    }
    }
}

static void update_overview_view(AppState *app)
{
    if (!app->overview_window || !IsWindow(app->overview_window)) {
        return;
    }

    SYSTEMTIME now = app->last_poll_time;
    wchar_t text[512];
    if (!app->has_poll_time) {
        GetLocalTime(&now);
    }
    if (app->gpu_ok) {
        wchar_t gpu_name[96];
        MultiByteToWideChar(CP_UTF8, 0, app->gpu.name, -1, gpu_name,
                            (int)(sizeof(gpu_name) / sizeof(gpu_name[0])));
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 L"%s · %d °C · %d controller%s · Updated %02d:%02d:%02d",
                 gpu_name[0] ? gpu_name : L"NVIDIA GPU", app->gpu.temperature_c,
                 app->device_count, app->device_count == 1 ? L"" : L"s",
                 now.wHour, now.wMinute, now.wSecond);
    } else {
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 L"NVIDIA GPU unavailable · %d controller%s · Updated %02d:%02d:%02d",
                 app->device_count, app->device_count == 1 ? L"" : L"s",
                 now.wHour, now.wMinute, now.wSecond);
    }
    SetWindowTextW(app->overview_gpu_label, text);

    int fan_rows = app->device_count * CORSAIR_FAN_COUNT;
    int sensor_rows = app->device_count;
    overview_set_row_count(app->overview_fan_list, fan_rows);
    overview_set_row_count(app->overview_sensor_list, sensor_rows);
    InvalidateRect(app->overview_fan_list, NULL, TRUE);
    InvalidateRect(app->overview_sensor_list, NULL, TRUE);
}

static const wchar_t *fan_profile_name(int mode_index)
{
    switch (mode_index) {
    case FAN_MODE_PWM_INDEX:
        return L"PWM";
    case FAN_MODE_DC_INDEX:
        return L"DC";
    case FAN_MODE_OFF_INDEX:
        return L"Off";
    case FAN_MODE_NVIDIA_INDEX:
        return L"NVIDIA";
    default:
        return L"Detected";
    }
}

static void layout_overview_controls(AppState *app, int width, int height)
{
    int m = ui_px(14);
    int gap = ui_px(12);
    int gpu_h = ui_px(22);
    int top = m + gpu_h + ui_px(8);
    int available = height - top - m - gap;
    if (available < ui_px(200)) {
        available = ui_px(200);
    }
    int sensor_h = available * 35 / 100;
    int fan_h = available - sensor_h - gap;
    int panel_w = width - m * 2;

    MoveWindow(app->overview_gpu_label, m, m, panel_w, gpu_h, TRUE);
    MoveWindow(app->overview_fans_panel, m, top, panel_w, fan_h, TRUE);
    MoveWindow(app->overview_fan_heading, m + ui_px(12), top + ui_px(10),
               ui_px(200), ui_px(16), TRUE);
    MoveWindow(app->overview_fan_list, ui_px(12), ui_px(34), panel_w - ui_px(24),
               fan_h - ui_px(44), TRUE);

    int sensor_top = top + fan_h + gap;
    MoveWindow(app->overview_sensor_panel, m, sensor_top, panel_w, sensor_h, TRUE);
    MoveWindow(app->overview_sensor_heading, m + ui_px(12), sensor_top + ui_px(10),
               ui_px(300), ui_px(16), TRUE);
    MoveWindow(app->overview_sensor_list, ui_px(12), ui_px(34),
               panel_w - ui_px(24), sensor_h - ui_px(44), TRUE);
}

static void create_overview_controls(AppState *app, HWND hwnd)
{
    UiTheme *t = ui_theme();

    app->overview_gpu_label = make_child(hwnd, WC_STATICW, L"", SS_LEFT, 14, 14, 1000, 22, 0);
    ui_register_font_role(app->overview_gpu_label, UI_FONT_MONO);
    ui_register_ctrl(app->overview_gpu_label, UI_BG_INK, t->text);

    app->overview_fans_panel = ui_make_control(hwnd, UI_CLASS_PANEL, 0, 14, 44, 1000, 400, 0);
    app->overview_fan_heading =
        make_child(hwnd, WC_STATICW, L"FANS", SS_LEFT, 26, 54, 200, 16, 0);
    ui_register_font_role(app->overview_fan_heading, UI_FONT_HEADING);
    ui_register_ctrl(app->overview_fan_heading, UI_BG_PANEL, t->dim);

    app->overview_fan_list = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_NOSORTHEADER,
        12, 34, 976, 360, app->overview_fans_panel, NULL, GetModuleHandleW(NULL),
        NULL);
    ui_register_font_role(app->overview_fan_list, UI_FONT_BODY);
    ui_register_ctrl(app->overview_fan_list, UI_BG_INK, t->text);
    ui_dark_listview(app->overview_fan_list, (1u << 3) | (1u << 5),
                     overview_fan_text, app);
    ui_panel_attach_list(app->overview_fans_panel, app->overview_fan_list);
    overview_add_column(app->overview_fan_list, 0, L"Controller", ui_px(240));
    overview_add_column(app->overview_fan_list, 1, L"Channel", ui_px(64));
    overview_add_column(app->overview_fan_list, 2, L"HW Mode", ui_px(110));
    overview_add_column(app->overview_fan_list, 3, L"RPM", ui_px(96));
    overview_add_column(app->overview_fan_list, 4, L"Profile", ui_px(96));
    overview_add_column(app->overview_fan_list, 5, L"Target", ui_px(72));

    app->overview_sensor_panel = ui_make_control(hwnd, UI_CLASS_PANEL, 0, 14, 500, 1000, 150, 0);
    app->overview_sensor_heading = make_child(hwnd, WC_STATICW,
                                              L"SENSORS & CONTROLLER STATE", SS_LEFT,
                                              26, 510, 300, 16, 0);
    ui_register_font_role(app->overview_sensor_heading, UI_FONT_HEADING);
    ui_register_ctrl(app->overview_sensor_heading, UI_BG_PANEL, t->dim);

    app->overview_sensor_list = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
            LVS_NOSORTHEADER,
        12, 34, 976, 120, app->overview_sensor_panel, NULL,
        GetModuleHandleW(NULL), NULL);
    ui_register_font_role(app->overview_sensor_list, UI_FONT_BODY);
    ui_register_ctrl(app->overview_sensor_list, UI_BG_INK, t->text);
    ui_dark_listview(app->overview_sensor_list,
                     (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) |
                         (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10),
                     overview_sensor_text, app);
    ui_panel_attach_list(app->overview_sensor_panel, app->overview_sensor_list);
    overview_add_column(app->overview_sensor_list, 0, L"Controller", ui_px(240));
    overview_add_column(app->overview_sensor_list, 1, L"State", ui_px(240));
    overview_add_column(app->overview_sensor_list, 2, L"Firmware", ui_px(84));
    overview_add_column(app->overview_sensor_list, 3, L"Bootloader", ui_px(84));
    overview_add_column(app->overview_sensor_list, 4, L"T1", ui_px(72));
    overview_add_column(app->overview_sensor_list, 5, L"T2", ui_px(72));
    overview_add_column(app->overview_sensor_list, 6, L"T3", ui_px(72));
    overview_add_column(app->overview_sensor_list, 7, L"T4", ui_px(72));
    overview_add_column(app->overview_sensor_list, 8, L"+12V", ui_px(72));
    overview_add_column(app->overview_sensor_list, 9, L"+5V", ui_px(64));
    overview_add_column(app->overview_sensor_list, 10, L"+3.3V", ui_px(68));

    /* Same Z-order rule as the main window: panels at the bottom. */
    SetWindowPos(app->overview_fans_panel, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(app->overview_sensor_panel, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);
}

static void show_overview_window(AppState *app)
{
    if (app->overview_window && IsWindow(app->overview_window)) {
        ShowWindow(app->overview_window, IsIconic(app->overview_window) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(app->overview_window);
        update_overview_view(app);
        return;
    }

    HWND hwnd = CreateWindowExW(
        0, L"CorsairNvidiaFanControlOverview", APP_TITLE L" · All Controllers",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        1240, 720, app->hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) {
        set_status(app, L"Could not open controller overview.");
        return;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
}

static LRESULT CALLBACK overview_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                          LPARAM lparam)
{
    AppState *app = &g_app;

    switch (msg) {
    case WM_CREATE: {
        app->overview_window = hwnd;
        create_overview_controls(app, hwnd);
        RECT client;
        GetClientRect(hwnd, &client);
        layout_overview_controls(app, client.right, client.bottom);
        update_overview_view(app);
        return 0;
    }

    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            layout_overview_controls(app, LOWORD(lparam), HIWORD(lparam));
        }
        return 0;

    case WM_DPICHANGED: {
        RECT *rect = (RECT *)lparam;
        SetWindowPos(hwnd, NULL, rect->left, rect->top, rect->right - rect->left,
                     rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ui_set_dpi(HIWORD(wparam));
        RECT client;
        GetClientRect(hwnd, &client);
        layout_overview_controls(app, client.right, client.bottom);
        ui_apply_fonts(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO *info = (MINMAXINFO *)lparam;
        info->ptMinTrackSize.x = ui_px(900);
        info->ptMinTrackSize.y = ui_px(540);
        return 0;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        return (LRESULT)ui_handle_ctrl_color(hwnd, (HDC)wparam, (HWND)lparam);

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        app->overview_window = NULL;
        app->overview_gpu_label = NULL;
        app->overview_fans_panel = NULL;
        app->overview_fan_heading = NULL;
        app->overview_fan_list = NULL;
        app->overview_sensor_panel = NULL;
        app->overview_sensor_heading = NULL;
        app->overview_sensor_list = NULL;
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* ===================================================== main window layout */

static void create_main_controls(AppState *app, HWND hwnd)
{
    UiTheme *t = ui_theme();

    /* Header ----------------------------------------------------------- */
    app->title_label = make_child(hwnd, WC_STATICW, L"FAN CONTROL", SS_LEFT,
                                  ui_px(14), ui_px(12), ui_px(360), ui_px(24), 0);
    ui_register_font_role(app->title_label, UI_FONT_BOLD);
    ui_register_ctrl(app->title_label, UI_BG_INK, t->text);
    app->device_subtitle = make_child(hwnd, WC_STATICW, L"No devices detected",
                                      SS_LEFT, ui_px(14), ui_px(36), ui_px(420),
                                      ui_px(18), 0);
    ui_register_font_role(app->device_subtitle, UI_FONT_CAPTION);
    ui_register_ctrl(app->device_subtitle, UI_BG_INK, t->dim);
    app->gpu_caption = make_child(hwnd, WC_STATICW, L"GPU CORE", SS_RIGHT,
                                  ui_px(400), ui_px(12), ui_px(150), ui_px(14), 0);
    ui_register_font_role(app->gpu_caption, UI_FONT_CAPTION);
    ui_register_ctrl(app->gpu_caption, UI_BG_INK, t->dim);
    app->gpu_readout = make_child(hwnd, WC_STATICW, L"— °C", SS_RIGHT,
                                  ui_px(400), ui_px(26), ui_px(150), ui_px(34), 0);
    ui_register_font_role(app->gpu_readout, UI_FONT_VALUE);
    ui_register_ctrl(app->gpu_readout, UI_BG_INK, t->text);
    app->status_dot = ui_make_control(hwnd, UI_CLASS_DOT, 0, ui_px(400), ui_px(44),
                                      ui_px(12), ui_px(12), 0);

    /* Left column ------------------------------------------------------- */
    app->fans_panel = ui_make_control(hwnd, UI_CLASS_PANEL, 0, ui_px(14), ui_px(74),
                                      ui_px(400), ui_px(332), 0);
    app->fans_heading = make_child(hwnd, WC_STATICW, L"FANS", SS_LEFT,
                                   ui_px(26), ui_px(84), ui_px(120), ui_px(16), 0);
    ui_register_font_role(app->fans_heading, UI_FONT_HEADING);
    ui_register_ctrl(app->fans_heading, UI_BG_PANEL, t->dim);

    static const wchar_t *mode_names[5] = {
        L"Detected", L"PWM", L"DC", L"Off", L"NVIDIA"
    };
    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        wchar_t label[32];
        swprintf(label, sizeof(label) / sizeof(label[0]), L"Fan %d", i + 1);
        app->fan_labels[i] = make_child(hwnd, WC_STATICW, label, SS_LEFT, 40, 90, 40, 18, 0);
        ui_register_font_role(app->fan_labels[i], UI_FONT_CAPTION);
        ui_register_ctrl(app->fan_labels[i], UI_BG_PANEL, t->dim);

        app->fan_mode[i] = ui_make_control(hwnd, UI_CLASS_COMBO, WS_TABSTOP, 60, 80, 84, 26,
                                           IDC_MODE_BASE + i);
        ui_combo_set_items(app->fan_mode[i], 5, mode_names);
        ui_combo_set_selected(app->fan_mode[i], FAN_MODE_DETECTED_INDEX);

        app->fan_rpm[i] = make_child(hwnd, WC_STATICW, L"· · · rpm", SS_LEFT,
                                     ui_px(150), ui_px(90), ui_px(100), ui_px(18),
                                     IDC_RPM_BASE + i);
        ui_register_font_role(app->fan_rpm[i], UI_FONT_MONO);
        ui_register_ctrl(app->fan_rpm[i], UI_BG_PANEL, t->text);

        app->fan_slider[i] = ui_make_control(hwnd, UI_CLASS_SLIDER, WS_TABSTOP,
                                             ui_px(250), ui_px(82), ui_px(150), ui_px(28),
                                             IDC_DUTY_BASE + i);
        ui_slider_set(app->fan_slider[i], 50);

        app->fan_duty[i] = make_child(hwnd, WC_STATICW, L"50%", SS_RIGHT,
                                      ui_px(560), ui_px(90), ui_px(44), ui_px(18), 0);
        ui_register_font_role(app->fan_duty[i], UI_FONT_MONO_BOLD);
        ui_register_ctrl(app->fan_duty[i], UI_BG_PANEL, t->text);

        app->fan_apply[i] = ui_make_control(hwnd, UI_CLASS_BUTTON, WS_TABSTOP,
                                            ui_px(610), ui_px(80), ui_px(64), ui_px(26),
                                            IDC_APPLY_BASE + i);
        SetWindowTextW(app->fan_apply[i], L"Apply");
    }

    app->sensors_panel = ui_make_control(hwnd, UI_CLASS_PANEL, 0, ui_px(14),
                                         ui_px(418), ui_px(400), ui_px(92), 0);
    app->sensors_heading = make_child(hwnd, WC_STATICW, L"SENSORS", SS_LEFT, ui_px(26),
                                      ui_px(428), ui_px(120), ui_px(16), 0);
    ui_register_font_role(app->sensors_heading, UI_FONT_HEADING);
    ui_register_ctrl(app->sensors_heading, UI_BG_PANEL, t->dim);

    for (int i = 0; i < CORSAIR_TEMP_COUNT; ++i) {
        wchar_t label[32];
        swprintf(label, sizeof(label) / sizeof(label[0]), L"T%d", i + 1);
        app->chip_labels_temp[i] = make_child(hwnd, WC_STATICW, label, SS_LEFT, 40, 452,
                                              40, 14, 0);
        ui_register_font_role(app->chip_labels_temp[i], UI_FONT_CAPTION);
        ui_register_ctrl(app->chip_labels_temp[i], UI_BG_PANEL, t->dim);
        app->temp_label[i] = make_child(hwnd, WC_STATICW, L"N/A", SS_LEFT, 40, 470, 80,
                                        ui_px(18), IDC_TEMP_BASE + i);
        ui_register_font_role(app->temp_label[i], UI_FONT_MONO);
        ui_register_ctrl(app->temp_label[i], UI_BG_PANEL, t->text);
    }

    app->rails_panel = ui_make_control(hwnd, UI_CLASS_PANEL, 0, ui_px(14),
                                       ui_px(522), ui_px(400), ui_px(92), 0);
    app->rails_heading = make_child(hwnd, WC_STATICW, L"RAILS", SS_LEFT, ui_px(26),
                                    ui_px(532), ui_px(120), ui_px(16), 0);
    ui_register_font_role(app->rails_heading, UI_FONT_HEADING);
    ui_register_ctrl(app->rails_heading, UI_BG_PANEL, t->dim);

    static const wchar_t *volt_labels[CORSAIR_VOLT_COUNT] = { L"+12V", L"+5V", L"+3.3V" };
    for (int i = 0; i < CORSAIR_VOLT_COUNT; ++i) {
        app->chip_labels_volt[i] = make_child(hwnd, WC_STATICW, volt_labels[i], SS_LEFT,
                                              40, ui_px(556), ui_px(40), ui_px(14), 0);
        ui_register_font_role(app->chip_labels_volt[i], UI_FONT_CAPTION);
        ui_register_ctrl(app->chip_labels_volt[i], UI_BG_PANEL, t->dim);
        app->volt_label[i] = make_child(hwnd, WC_STATICW, L"N/A", SS_LEFT, 40,
                                        ui_px(574), ui_px(70), ui_px(18),
                                        IDC_VOLT_BASE + i);
        ui_register_font_role(app->volt_label[i], UI_FONT_MONO);
        ui_register_ctrl(app->volt_label[i], UI_BG_PANEL, t->text);
    }

    /* Right column ------------------------------------------------------ */
    int x_r = ui_px(430);
    app->curve_panel = ui_make_control(hwnd, UI_CLASS_PANEL, 0, x_r, ui_px(74),
                                       ui_px(264), ui_px(254), 0);
    app->curve_heading = make_child(hwnd, WC_STATICW, L"GPU CURVE", SS_LEFT,
                                    x_r + ui_px(12), ui_px(84), ui_px(160), ui_px(16),
                                    0);
    ui_register_font_role(app->curve_heading, UI_FONT_HEADING);
    ui_register_ctrl(app->curve_heading, UI_BG_PANEL, t->dim);

    app->gpu_status_label = make_child(hwnd, WC_STATICW, L"NVIDIA: unavailable",
                                       SS_LEFT, x_r + ui_px(12), ui_px(104),
                                       ui_px(240), ui_px(16), IDC_GPU_STATUS);
    ui_register_font_role(app->gpu_status_label, UI_FONT_CAPTION);
    ui_register_ctrl(app->gpu_status_label, UI_BG_PANEL, t->dim);

    app->curve_graph = ui_make_control(hwnd, UI_CLASS_CURVE, 0, x_r + ui_px(12),
                                       ui_px(124), ui_px(240), ui_px(128), 0);

    app->low_high_labels[0] = make_child(hwnd, WC_STATICW, L"LOW", SS_LEFT,
                                         x_r + ui_px(12), ui_px(186), ui_px(30),
                                         ui_px(14), 0);
    ui_register_font_role(app->low_high_labels[0], UI_FONT_CAPTION);
    ui_register_ctrl(app->low_high_labels[0], UI_BG_PANEL, t->dim);
    app->gpu_temp_low_edit = make_child(hwnd, WC_EDITW, L"40", ES_NUMBER | WS_TABSTOP,
                                        x_r + ui_px(48), ui_px(187), ui_px(48), ui_px(20),
                                        IDC_GPU_TEMP_LOW);
    ui_register_font_role(app->gpu_temp_low_edit, UI_FONT_MONO);
    ui_register_ctrl(app->gpu_temp_low_edit, UI_BG_INK, t->text);
    app->gpu_duty_low_edit = make_child(hwnd, WC_EDITW, L"25", ES_NUMBER | WS_TABSTOP,
                                        x_r + ui_px(106), ui_px(187), ui_px(48), ui_px(20),
                                        IDC_GPU_DUTY_LOW);
    ui_register_font_role(app->gpu_duty_low_edit, UI_FONT_MONO);
    ui_register_ctrl(app->gpu_duty_low_edit, UI_BG_INK, t->text);
    app->pct_labels[0] = make_child(hwnd, WC_STATICW, L"%", SS_LEFT,
                                    x_r + ui_px(160), ui_px(187), ui_px(20), ui_px(14),
                                    0);
    ui_register_font_role(app->pct_labels[0], UI_FONT_CAPTION);
    ui_register_ctrl(app->pct_labels[0], UI_BG_PANEL, t->dim);

    app->low_high_labels[1] = make_child(hwnd, WC_STATICW, L"HIGH", SS_LEFT,
                                         x_r + ui_px(12), ui_px(220), ui_px(30),
                                         ui_px(14), 0);
    ui_register_font_role(app->low_high_labels[1], UI_FONT_CAPTION);
    ui_register_ctrl(app->low_high_labels[1], UI_BG_PANEL, t->dim);
    app->gpu_temp_high_edit = make_child(hwnd, WC_EDITW, L"80", ES_NUMBER | WS_TABSTOP,
                                         x_r + ui_px(48), ui_px(221), ui_px(48), ui_px(20),
                                         IDC_GPU_TEMP_HIGH);
    ui_register_font_role(app->gpu_temp_high_edit, UI_FONT_MONO);
    ui_register_ctrl(app->gpu_temp_high_edit, UI_BG_INK, t->text);
    app->gpu_duty_high_edit = make_child(hwnd, WC_EDITW, L"100", ES_NUMBER | WS_TABSTOP,
                                         x_r + ui_px(106), ui_px(221), ui_px(48), ui_px(20),
                                         IDC_GPU_DUTY_HIGH);
    ui_register_font_role(app->gpu_duty_high_edit, UI_FONT_MONO);
    ui_register_ctrl(app->gpu_duty_high_edit, UI_BG_INK, t->text);
    app->pct_labels[1] = make_child(hwnd, WC_STATICW, L"%", SS_LEFT,
                                    x_r + ui_px(160), ui_px(221), ui_px(20), ui_px(14),
                                    0);
    ui_register_font_role(app->pct_labels[1], UI_FONT_CAPTION);
    ui_register_ctrl(app->pct_labels[1], UI_BG_PANEL, t->dim);

    app->controller_panel = ui_make_control(hwnd, UI_CLASS_PANEL, 0, x_r,
                                            ui_px(340), ui_px(264), ui_px(150), 0);
    app->controller_heading = make_child(hwnd, WC_STATICW, L"CONTROLLER", SS_LEFT,
                                         x_r + ui_px(12), ui_px(350), ui_px(160),
                                         ui_px(16), 0);
    ui_register_font_role(app->controller_heading, UI_FONT_HEADING);
    ui_register_ctrl(app->controller_heading, UI_BG_PANEL, t->dim);

    static const wchar_t *row_labels[4] = {
        L"State", L"Firmware", L"Bootloader", L"Error"
    };
    for (int i = 0; i < 4; ++i) {
        int y = 376 + i * 24;
        app->ctrl_row_labels[i] = make_child(hwnd, WC_STATICW, row_labels[i], SS_LEFT,
                                             x_r + ui_px(12), ui_px(y), ui_px(80),
                                             ui_px(16), 0);
        ui_register_font_role(app->ctrl_row_labels[i], UI_FONT_CAPTION);
        ui_register_ctrl(app->ctrl_row_labels[i], UI_BG_PANEL, t->dim);
    }

    app->ctrl_state_label = make_child(hwnd, WC_STATICW, L"Offline", SS_LEFT,
                                       x_r + ui_px(96), ui_px(376), ui_px(156), ui_px(16), 0);
    ui_register_font_role(app->ctrl_state_label, UI_FONT_BOLD);
    ui_register_ctrl(app->ctrl_state_label, UI_BG_PANEL, t->text);

    app->ctrl_fw_label = make_child(hwnd, WC_STATICW, L"—", SS_LEFT, x_r + ui_px(96),
                                    ui_px(400), ui_px(156), ui_px(16), 0);
    ui_register_font_role(app->ctrl_fw_label, UI_FONT_MONO);
    ui_register_ctrl(app->ctrl_fw_label, UI_BG_PANEL, t->text);

    app->ctrl_bl_label = make_child(hwnd, WC_STATICW, L"—", SS_LEFT, x_r + ui_px(96),
                                    ui_px(424), ui_px(156), ui_px(16), 0);
    ui_register_font_role(app->ctrl_bl_label, UI_FONT_MONO);
    ui_register_ctrl(app->ctrl_bl_label, UI_BG_PANEL, t->text);

    app->ctrl_err_label = make_child(hwnd, WC_STATICW, L"", SS_LEFT, x_r + ui_px(12),
                                     ui_px(466), ui_px(240), ui_px(14), 0);
    ui_register_font_role(app->ctrl_err_label, UI_FONT_CAPTION);
    ui_register_ctrl(app->ctrl_err_label, UI_BG_PANEL, t->warn);

    app->options_panel = ui_make_control(hwnd, UI_CLASS_PANEL, 0, x_r,
                                         ui_px(502), ui_px(264), ui_px(98), 0);
    app->options_heading = make_child(hwnd, WC_STATICW, L"OPTIONS", SS_LEFT,
                                      x_r + ui_px(12), ui_px(512), ui_px(160),
                                      ui_px(16), 0);
    ui_register_font_role(app->options_heading, UI_FONT_HEADING);
    ui_register_ctrl(app->options_heading, UI_BG_PANEL, t->dim);

    app->autostart_checkbox = make_child(hwnd, WC_BUTTONW, L"Start with Windows",
                                         BS_AUTOCHECKBOX | WS_TABSTOP, x_r + ui_px(12),
                                         ui_px(536), ui_px(160), ui_px(20),
                                         IDC_AUTOSTART);
    ui_register_font_role(app->autostart_checkbox, UI_FONT_BODY);
    ui_register_ctrl(app->autostart_checkbox, UI_BG_PANEL, t->text);
    SendMessageW(app->autostart_checkbox, BM_SETCHECK,
                 is_autostart_enabled() ? BST_CHECKED : BST_UNCHECKED, 0);

    app->save_btn = ui_make_control(hwnd, UI_CLASS_BUTTON, WS_TABSTOP, x_r + ui_px(12),
                                    ui_px(562), ui_px(240), ui_px(28), IDC_SAVE_SETTINGS);
    SetWindowTextW(app->save_btn, L"Save settings");
    ui_button_set_variant(app->save_btn, UI_BTN_PRIMARY);

    /* Footer ------------------------------------------------------------- */
    app->device_combo = ui_make_control(hwnd, UI_CLASS_COMBO, WS_TABSTOP, ui_px(14),
                                        ui_px(616), ui_px(210), ui_px(28),
                                        IDC_DEVICE_COMBO);
    app->scan_btn = ui_make_control(hwnd, UI_CLASS_BUTTON, WS_TABSTOP, ui_px(234),
                                    ui_px(616), ui_px(70), ui_px(28), IDC_SCAN);
    SetWindowTextW(app->scan_btn, L"Scan");
    app->refresh_btn = ui_make_control(hwnd, UI_CLASS_BUTTON, WS_TABSTOP, ui_px(314),
                                       ui_px(616), ui_px(80), ui_px(28), IDC_REFRESH);
    SetWindowTextW(app->refresh_btn, L"Refresh");
    app->apply_all_btn = ui_make_control(hwnd, UI_CLASS_BUTTON, WS_TABSTOP, ui_px(404),
                                         ui_px(616), ui_px(84), ui_px(28),
                                         IDC_APPLY_ALL);
    SetWindowTextW(app->apply_all_btn, L"Apply all");
    app->overview_btn = ui_make_control(hwnd, UI_CLASS_BUTTON, WS_TABSTOP, ui_px(498),
                                        ui_px(616), ui_px(84), ui_px(28), IDC_OVERVIEW);
    SetWindowTextW(app->overview_btn, L"Overview");

    app->status_label = make_child(hwnd, WC_STATICW, L"Scan for devices.", SS_LEFT,
                                   ui_px(14), ui_px(650), ui_px(700), ui_px(16),
                                   IDC_STATUS);
    ui_register_font_role(app->status_label, UI_FONT_CAPTION);
    ui_register_ctrl(app->status_label, UI_BG_INK, t->dim);

    /* The GPU-curve edit controls sit inside the curve panel; raise them to
     * the top of the Z-order so their background chips / the curve graph do
     * not intercept mouse input. */
    SetWindowPos(app->gpu_temp_low_edit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(app->gpu_duty_low_edit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(app->gpu_temp_high_edit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(app->gpu_duty_high_edit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    InvalidateRect(app->gpu_temp_low_edit, NULL, FALSE);
    InvalidateRect(app->gpu_duty_low_edit, NULL, FALSE);
    InvalidateRect(app->gpu_temp_high_edit, NULL, FALSE);
    InvalidateRect(app->gpu_duty_high_edit, NULL, FALSE);

    /* Keep the background panels at the very bottom of the Z-order. With
     * WS_CLIPSIBLINGS they then clip out every sibling above them, so a
     * panel repaint can never erase the pixels of the controls sitting on
     * top of it (fan combos/sliders/buttons, curve graph, LOW/HIGH edits). */
    SetWindowPos(app->fans_panel, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(app->sensors_panel, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(app->rails_panel, HWND_BOTTOM, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(app->curve_panel, HWND_BOTTOM, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(app->controller_panel, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(app->options_panel, HWND_BOTTOM, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE);

    load_global_settings(app);
    update_curve_points(app);
    refresh_gpu_status(app);
    update_controls_enabled(app);
    update_device_subtitle(app);
}

static void layout_main(AppState *app)
{
    RECT client;
    GetClientRect(app->hwnd, &client);
    int w = client.right;
    int h = client.bottom;
    int m = ui_px(14);
    int gap = ui_px(12);

    /* Header. */
    int readout_w = ui_px(150);
    MoveWindow(app->title_label, m, ui_px(12), ui_px(360), ui_px(24), TRUE);
    MoveWindow(app->device_subtitle, m, ui_px(36), ui_px(420), ui_px(18), TRUE);
    MoveWindow(app->gpu_caption, w - m - readout_w, ui_px(12), readout_w, ui_px(14),
               TRUE);
    MoveWindow(app->gpu_readout, w - m - readout_w, ui_px(26), readout_w, ui_px(34),
               TRUE);
    MoveWindow(app->status_dot, w - m - readout_w - ui_px(22), ui_px(44), ui_px(12),
               ui_px(12), TRUE);

    /* Body. */
    int body_top = ui_px(74);
    int footer_h = ui_px(52);
    int body_bottom = h - m - footer_h - ui_px(8);
    int body_h = body_bottom - body_top;
    int right_w = ui_px(264);
    int left_w = w - m * 2 - right_w - gap;
    int x_r = m + left_w + gap;

    int sens_h = body_h >= ui_px(552) ? ui_px(92) : ui_px(80);
    int rail_h = sens_h;
    int fans_h = body_h - sens_h - rail_h - gap * 2;
    if (fans_h < ui_px(260)) {
        fans_h = ui_px(260);
    }
    int row_area = fans_h - ui_px(46);
    int row_h = row_area / CORSAIR_FAN_COUNT;
    if (row_h < ui_px(34)) {
        row_h = ui_px(34);
    }

    MoveWindow(app->fans_panel, m, body_top, left_w, fans_h, TRUE);
    MoveWindow(app->fans_heading, m + ui_px(12), body_top + ui_px(10), ui_px(120),
               ui_px(16), TRUE);
    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        int y = body_top + ui_px(38) + i * row_h;
        MoveWindow(app->fan_labels[i], m + ui_px(12), y + (row_h - ui_px(18)) / 2,
                   ui_px(40), ui_px(18), TRUE);
        MoveWindow(app->fan_mode[i], m + ui_px(54), y + (row_h - ui_px(26)) / 2,
                   ui_px(84), ui_px(26), TRUE);
        MoveWindow(app->fan_rpm[i], m + ui_px(146), y + (row_h - ui_px(18)) / 2,
                   ui_px(100), ui_px(18), TRUE);
        MoveWindow(app->fan_apply[i], m + left_w - ui_px(12) - ui_px(64),
                   y + (row_h - ui_px(26)) / 2, ui_px(64), ui_px(26), TRUE);
        MoveWindow(app->fan_duty[i],
                   m + left_w - ui_px(12) - ui_px(64) - ui_px(8) - ui_px(44),
                   y + (row_h - ui_px(18)) / 2, ui_px(44), ui_px(18), TRUE);
        int slider_x = m + ui_px(246);
        int slider_w = (m + left_w - ui_px(12) - ui_px(64) - ui_px(8) - ui_px(44) -
                        ui_px(8)) -
                       slider_x;
        MoveWindow(app->fan_slider[i], slider_x, y + (row_h - ui_px(28)) / 2, slider_w,
                   ui_px(28), TRUE);
    }

    int sensors_top = body_top + fans_h + gap;
    MoveWindow(app->sensors_panel, m, sensors_top, left_w, sens_h, TRUE);
    MoveWindow(app->sensors_heading, m + ui_px(12), sensors_top + ui_px(10), ui_px(120),
               ui_px(16), TRUE);
    int chip_h = sens_h - ui_px(46);
    int chip_w = (left_w - ui_px(24) - ui_px(30)) / CORSAIR_TEMP_COUNT;
    for (int i = 0; i < CORSAIR_TEMP_COUNT; ++i) {
        int x = m + ui_px(12) + i * (chip_w + ui_px(10));
        MoveWindow(app->chip_panels_temp[i], x, sensors_top + ui_px(36), chip_w, chip_h,
                   TRUE);
        MoveWindow(app->chip_labels_temp[i], x + ui_px(8), sensors_top + ui_px(41),
                   ui_px(40), ui_px(14), TRUE);
        MoveWindow(app->temp_label[i], x + ui_px(8), sensors_top + ui_px(41) + ui_px(16),
                   chip_w - ui_px(16), ui_px(18), TRUE);
    }

    int rails_top = sensors_top + sens_h + gap;
    MoveWindow(app->rails_panel, m, rails_top, left_w, rail_h, TRUE);
    MoveWindow(app->rails_heading, m + ui_px(12), rails_top + ui_px(10), ui_px(120),
               ui_px(16), TRUE);
    int chip_w3 = (left_w - ui_px(24) - ui_px(20)) / CORSAIR_VOLT_COUNT;
    for (int i = 0; i < CORSAIR_VOLT_COUNT; ++i) {
        int x = m + ui_px(12) + i * (chip_w3 + ui_px(10));
        MoveWindow(app->chip_panels_volt[i], x, rails_top + ui_px(36), chip_w3, chip_h,
                   TRUE);
        MoveWindow(app->chip_labels_volt[i], x + ui_px(8), rails_top + ui_px(41),
                   ui_px(40), ui_px(14), TRUE);
        MoveWindow(app->volt_label[i], x + ui_px(8), rails_top + ui_px(41) + ui_px(16),
                   chip_w3 - ui_px(16), ui_px(18), TRUE);
    }

    /* Right column. */
    int curve_h = body_h * 52 / 100;
    if (curve_h < ui_px(236)) {
        curve_h = ui_px(236);
    }
    int ctrl_h = ui_px(150);
    int opts_h = body_h - curve_h - ctrl_h - gap * 2;
    if (opts_h < ui_px(98)) {
        opts_h = ui_px(98);
    }

    MoveWindow(app->curve_panel, x_r, body_top, right_w, curve_h, TRUE);
    MoveWindow(app->curve_heading, x_r + ui_px(12), body_top + ui_px(10), ui_px(160),
               ui_px(16), TRUE);
    MoveWindow(app->gpu_status_label, x_r + ui_px(12), body_top + ui_px(30),
               right_w - ui_px(24), ui_px(16), TRUE);
    MoveWindow(app->curve_graph, x_r + ui_px(12), body_top + ui_px(50),
               right_w - ui_px(24), curve_h - ui_px(190), TRUE);
    MoveWindow(app->low_high_labels[0], x_r + ui_px(12), body_top + curve_h - ui_px(67),
               ui_px(30), ui_px(14), TRUE);
    MoveWindow(app->pct_labels[0], x_r + ui_px(160), body_top + curve_h - ui_px(67),
               ui_px(20), ui_px(14), TRUE);
    MoveWindow(app->gpu_temp_low_edit, x_r + ui_px(48), body_top + curve_h - ui_px(65),
               ui_px(48), ui_px(20), TRUE);
    MoveWindow(app->gpu_duty_low_edit, x_r + ui_px(106), body_top + curve_h - ui_px(65),
               ui_px(48), ui_px(20), TRUE);
    MoveWindow(app->low_high_labels[1], x_r + ui_px(12), body_top + curve_h - ui_px(33),
               ui_px(30), ui_px(14), TRUE);
    MoveWindow(app->pct_labels[1], x_r + ui_px(160), body_top + curve_h - ui_px(33),
               ui_px(20), ui_px(14), TRUE);
    MoveWindow(app->gpu_temp_high_edit, x_r + ui_px(48), body_top + curve_h - ui_px(31),
               ui_px(48), ui_px(20), TRUE);
    MoveWindow(app->gpu_duty_high_edit, x_r + ui_px(106), body_top + curve_h - ui_px(31),
               ui_px(48), ui_px(20), TRUE);

    int controller_top = body_top + curve_h + gap;
    MoveWindow(app->controller_panel, x_r, controller_top, right_w, ctrl_h, TRUE);
    MoveWindow(app->controller_heading, x_r + ui_px(12), controller_top + ui_px(10),
               ui_px(160), ui_px(16), TRUE);
    for (int i = 0; i < 4; ++i) {
        MoveWindow(app->ctrl_row_labels[i], x_r + ui_px(12),
                   controller_top + ui_px(36) + i * ui_px(24), ui_px(80), ui_px(16),
                   TRUE);
    }
    MoveWindow(app->ctrl_state_label, x_r + ui_px(96), controller_top + ui_px(36),
               right_w - ui_px(108), ui_px(16), TRUE);
    MoveWindow(app->ctrl_fw_label, x_r + ui_px(96), controller_top + ui_px(60),
               right_w - ui_px(108), ui_px(16), TRUE);
    MoveWindow(app->ctrl_bl_label, x_r + ui_px(96), controller_top + ui_px(84),
               right_w - ui_px(108), ui_px(16), TRUE);
    MoveWindow(app->ctrl_err_label, x_r + ui_px(12), controller_top + ui_px(126),
               right_w - ui_px(24), ui_px(14), TRUE);

    int options_top = controller_top + ctrl_h + gap;
    MoveWindow(app->options_panel, x_r, options_top, right_w, opts_h, TRUE);
    MoveWindow(app->options_heading, x_r + ui_px(12), options_top + ui_px(10),
               ui_px(160), ui_px(16), TRUE);
    MoveWindow(app->autostart_checkbox, x_r + ui_px(12), options_top + ui_px(34),
               ui_px(180), ui_px(20), TRUE);
    MoveWindow(app->save_btn, x_r + ui_px(12), options_top + opts_h - ui_px(38),
               right_w - ui_px(24), ui_px(28), TRUE);

    /* Footer. */
    int footer_y = h - m - footer_h;
    MoveWindow(app->device_combo, m, footer_y, ui_px(210), ui_px(28), TRUE);
    MoveWindow(app->scan_btn, m + ui_px(220), footer_y, ui_px(70), ui_px(28), TRUE);
    MoveWindow(app->refresh_btn, m + ui_px(300), footer_y, ui_px(80), ui_px(28), TRUE);
    MoveWindow(app->apply_all_btn, m + ui_px(390), footer_y, ui_px(84), ui_px(28),
               TRUE);
    MoveWindow(app->overview_btn, m + ui_px(484), footer_y, ui_px(84), ui_px(28),
               TRUE);
    MoveWindow(app->status_label, m, footer_y + ui_px(34), w - m * 2, ui_px(16), TRUE);
}

/* ================================================================== main */

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
        create_main_controls(app, hwnd);
        RECT client;
        GetClientRect(hwnd, &client);
        layout_main(app);
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
        } else if (id == IDC_OVERVIEW) {
            show_overview_window(app);
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
                        snprintf(first_error, sizeof(first_error), "Controller %d: %s",
                                 i + 1, err);
                    }
                }
            }

            refresh_status(app);
            for (int i = 0; i < app->device_count; ++i) {
                if (!app->controllers[i].opened || app->controllers[i].last_error[0]) {
                    apply_ok = false;
                    if (!first_error[0]) {
                        snprintf(first_error, sizeof(first_error), "Controller %d: %s",
                                 i + 1,
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
            bool enabled =
                SendMessageW(app->autostart_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!set_autostart_enabled(enabled)) {
                SendMessageW(app->autostart_checkbox, BM_SETCHECK,
                             enabled ? BST_UNCHECKED : BST_CHECKED, 0);
                set_status(app, L"Autostart registry update failed.");
            } else {
                save_settings(app);
                set_status(app,
                           enabled ? L"Autostart enabled." : L"Autostart disabled.");
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
                update_overview_view(app);
            }
        } else if ((id == IDC_GPU_TEMP_LOW || id == IDC_GPU_DUTY_LOW ||
                    id == IDC_GPU_TEMP_HIGH || id == IDC_GPU_DUTY_HIGH) &&
                   HIWORD(wparam) == EN_KILLFOCUS) {
            update_curve_points(app);
            save_settings(app);
            update_overview_view(app);
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
                        update_overview_view(app);
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
        layout_main(app);
        return 0;

    case WM_DPICHANGED: {
        RECT *rect = (RECT *)lparam;
        SetWindowPos(hwnd, NULL, rect->left, rect->top, rect->right - rect->left,
                     rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ui_set_dpi(HIWORD(wparam));
        layout_main(app);
        ui_apply_fonts(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO *info = (MINMAXINFO *)lparam;
        info->ptMinTrackSize.x = ui_px(820);
        info->ptMinTrackSize.y = ui_px(690);
        return 0;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        return (LRESULT)ui_handle_ctrl_color(hwnd, (HDC)wparam, (HWND)lparam);

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
        if (app->overview_window && IsWindow(app->overview_window)) {
            DestroyWindow(app->overview_window);
        }
        close_all_controllers(app);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev_instance, LPSTR cmd_line,
                   int show_cmd)
{
    (void)prev_instance;

    /* Per-monitor DPI awareness (Windows 10+), fallback for older systems. */
    typedef BOOL (WINAPI *SetProcessDpiContextFn)(DPI_AWARENESS_CONTEXT);
    SetProcessDpiContextFn set_dpi_context =
        (SetProcessDpiContextFn)(void *)GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (set_dpi_context) {
        set_dpi_context((DPI_AWARENESS_CONTEXT)4 /* PER_MONITOR_AWARE_V2 */);
    } else {
        SetProcessDPIAware();
    }

    ui_init();
    UiTheme *t = ui_theme();

    INITCOMMONCONTROLSEX icc;
    WNDCLASSW wc;
    WNDCLASSW overview_wc;
    HWND hwnd;
    MSG msg;

    ZeroMemory(&g_app, sizeof(g_app));
    g_app.active_device_index = -1;
    g_app.taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");
    g_app.start_in_tray = strstr(cmd_line, "--tray") != NULL;

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"CorsairNvidiaFanControlWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hbrBackground = t->ink_brush;

    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"Could not register window class.", APP_TITLE, MB_ICONERROR);
        return 1;
    }

    overview_wc = wc;
    overview_wc.lpfnWndProc = overview_wnd_proc;
    overview_wc.lpszClassName = L"CorsairNvidiaFanControlOverview";
    if (!RegisterClassW(&overview_wc)) {
        MessageBoxW(NULL, L"Could not register window class.", APP_TITLE, MB_ICONERROR);
        return 1;
    }

    hwnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE,
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, 900, 720, NULL, NULL, instance,
                           NULL);
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
        if (!IsDialogMessageW(g_app.hwnd, &msg)) {
            DispatchMessageW(&msg);
        }
    }

    ui_destroy();
    return (int)msg.wParam;
}