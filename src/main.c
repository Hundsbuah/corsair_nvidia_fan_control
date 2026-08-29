#include "corsair_hid.h"
#include "gpu_stats.h"
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
#define IDC_AUTOSTART 905
#define IDC_SAVE_SETTINGS 906
#define IDC_UPDATE_INTERVAL 907
#define REFRESH_TIMER 1
#define TRAY_RETRY_TIMER 2
/* GPU telemetry readout rows (index order = row order in the panel). */
#define STAT_ROW_COUNT 6
/* Update (poll) interval: adjustable between UPDATE_MIN_MS and UPDATE_MAX_MS.
 * A slider maps position 0 (slowest, max ms) .. 100 (fastest, min ms). */
#define UPDATE_MIN_MS 500
#define UPDATE_MAX_MS 5000
#define UPDATE_DEFAULT_MS 5000
/* Sensor reads (corsair_refresh) run on every refresh tick, i.e. at the same
 * interval as the GUI refresh. A controller whose last refresh cycle failed
 * is skipped until every REFRESH_FAIL_RETRY_EVERY-th tick so a dead device
 * does not trigger a close + reopen + full initialize on every tick. */
#define REFRESH_FAIL_RETRY_EVERY 4
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
    bool refresh_failed;
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
    HWND update_label;   /* "Update" caption in the OPTIONS panel */
    HWND update_slider;  /* poll-interval slider (0.5 s .. 5 s) */
    HWND update_readout; /* "5.0 s" interval readout */
    int update_ms;       /* current poll interval in ms */
    HWND status_label;
    /* GPU curve */
    HWND curve_hint;
    HWND curve_hint2;
    /* GPU telemetry column */
    HWND stats_panel;
    HWND stats_heading;
    HWND stat_labels[STAT_ROW_COUNT];
    HWND stat_values[STAT_ROW_COUNT];
    HWND stats_meter;
    GpuStats gpu_stats;
    /* Panel headings / static labels (moved by layout) */
    HWND fans_heading;
    HWND sensors_heading;
    HWND rails_heading;
    HWND curve_heading;
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
    unsigned poll_counter;
    UINT taskbar_created_msg;
    bool start_in_tray;
    bool tray_added;
    bool tray_icon_wanted;
    bool allow_close;
} AppState;

static AppState g_app;

static int selected_device_index(AppState *app);
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

/* Derive a stable key that identifies the PHYSICAL controller, not the USB
 * port. Prefers the USB serial number (stable + unique across reboots and
 * port moves). Falls back to VID:PID plus the hardware prefix of the device
 * path (everything before the first '#', which excludes the port-dependent
 * instance ID). The prefix alone is constant for all HID devices (\\?\\HID),
 * so the VID/PID is included to keep serial-less controllers apart; two
 * identical serial-less devices (same VID/PID) can still collide. The
 * previous full-path hash changed when the controller was moved to a
 * different port, silently losing per-device settings. */
static void device_settings_key(const CorsairDeviceInfo *info, wchar_t *key,
                                size_t key_count)
{
    uint64_t hash = 1469598103934665603ULL;
    wchar_t ident[160];
    const wchar_t *text;

    if (info && info->serial[0]) {
        swprintf(ident, 160, L"usb:%04X:%04X:%s",
                 (unsigned)info->vendor_id, (unsigned)info->product_id,
                 info->serial);
        text = ident;
    } else {
        const wchar_t *path = info && info->path[0] ? info->path : L"unknown";
        wchar_t prefix[64];
        size_t i = 0;
        while (path[i] != L'\0' && path[i] != L'#' && i + 1 < 64) {
            prefix[i] = path[i];
            ++i;
        }
        prefix[i] = L'\0';
        /* The prefix before the first '#' is constant (\\?\\HID), so the
         * VID/PID carries the distinguishing part. */
        swprintf(ident, 160, L"hw:%04X:%04X:%s",
                 info ? (unsigned)info->vendor_id : 0,
                 info ? (unsigned)info->product_id : 0, prefix);
        text = ident;
    }

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

/* ========================================================= fake device */

static int g_fake_duty[CORSAIR_FAN_COUNT];

static bool fake_device_enabled(void)
{
    /* Win32 query (no allocation, avoids the deprecated CRT getenv): returns
     * the character count the value would occupy, or 0 if the variable is
     * unset. */
    return GetEnvironmentVariableA(FAKE_DEVICE_ENV, NULL, 0) > 0;
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
    static bool seeded;
    CorsairStatus *status = &app->controllers[device_index].device.status;

    if (!seeded) {
        srand((unsigned)GetTickCount());
        seeded = true;
    }

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
    /* Keep the random walk inside a plausible range so multi-hour fake
     * sessions do not drift into unrealistic temperatures. */
    if (status->temp_c[0] < 20.0) {
        status->temp_c[0] = 20.0;
    }
    if (status->temp_c[0] > 70.0) {
        status->temp_c[0] = 70.0;
    }
    if (status->temp_c[3] < 20.0) {
        status->temp_c[3] = 20.0;
    }
    if (status->temp_c[3] > 70.0) {
        status->temp_c[3] = 70.0;
    }
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

/* Parse "25:30,80:100" into point arrays; requires 2..UI_CURVE_MAX_POINTS
 * valid points (same limit the interactive editor enforces, so points can
 * never be silently dropped by ui_curve_set_points). */
static bool parse_curve_points(const wchar_t *text, int *temps, int *duties,
                               int *count)
{
    int n = 0;
    const wchar_t *p = text;

    while (*p != L'\0') {
        wchar_t *end1 = NULL;
        long temp_value = wcstol(p, &end1, 10);
        if (end1 == p || *end1 != L':') {
            return false;
        }
        wchar_t *end2 = NULL;
        long duty_value = wcstol(end1 + 1, &end2, 10);
        if (end2 == end1 + 1 || temp_value < 0 || temp_value > 100 ||
            duty_value < 0 || duty_value > 100 || n >= UI_CURVE_MAX_POINTS) {
            return false;
        }
        temps[n] = (int)temp_value;
        duties[n] = (int)duty_value;
        ++n;
        p = end2;
        while (*p == L' ') {
            ++p;
        }
        if (*p == L'\0') {
            break;
        }
        if (*p != L',') {
            return false;
        }
        ++p;
        while (*p == L' ') {
            ++p;
        }
    }

    *count = n;
    return n >= 2;
}

static void load_global_settings(AppState *app)
{
    HKEY key;
    app->update_ms = UPDATE_DEFAULT_MS;
    if (!settings_open_existing_key(SETTINGS_KEY, &key, KEY_QUERY_VALUE) &&
        !settings_open_existing_key(LEGACY_SETTINGS_KEY, &key, KEY_QUERY_VALUE)) {
        app->saved_device_index = 0;
        app->last_device_key[0] = L'\0';
        return;
    }

    app->update_ms = (int)settings_read_dword(key, L"UpdateMs", UPDATE_DEFAULT_MS);
    app->saved_device_index = (int)settings_read_dword(key, L"DeviceIndex", 0);
    settings_read_string(key, L"LastDeviceKey", app->last_device_key,
                         (DWORD)(sizeof(app->last_device_key) /
                                 sizeof(app->last_device_key[0])));
    /* Multi-point curve: GpuCurvePts ("25:30,80:100"). Without a saved or
     * valid curve the control defaults apply (25 deg C -> 30% and
     * 80 deg C -> 100%); legacy two-point settings are migrated once. */
    {
        wchar_t curve_pts[512];
        int temps[16];
        int duties[16];
        int point_count = 0;
        bool loaded = settings_read_string(
            key, L"GpuCurvePts", curve_pts,
            (DWORD)(sizeof(curve_pts) / sizeof(curve_pts[0]))) &&
                      parse_curve_points(curve_pts, temps, duties,
                                         &point_count);
        if (loaded) {
            ui_curve_set_points(app->curve_graph, point_count, temps, duties);
        } else {
            DWORD temp_low = 0;
            DWORD temp_high = 0;
            if (settings_try_read_dword(key, L"GpuTempLow", &temp_low) &&
                settings_try_read_dword(key, L"GpuTempHigh", &temp_high)) {
                int legacy_t[2] = { (int)temp_low, (int)temp_high };
                int legacy_d[2] = {
                    (int)settings_read_dword(key, L"GpuDutyLow", 30),
                    (int)settings_read_dword(key, L"GpuDutyHigh", 100)
                };
                ui_curve_set_points(app->curve_graph, 2, legacy_t, legacy_d);
            }
        }
    }

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

    settings_write_dword(key, L"UpdateMs", (DWORD)app->update_ms);
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

    {
        int temps[16];
        int duties[16];
        int point_count = ui_curve_get_points(app->curve_graph, &point_count,
                                              temps, duties);
        if (point_count >= 2) {
            wchar_t curve_pts[512];
            int pos = 0;
            for (int i = 0; i < point_count; ++i) {
                pos += swprintf(&curve_pts[pos], 16, L"%d:%d%s", temps[i],
                                duties[i], i + 1 < point_count ? L"," : L"");
            }
            settings_write_string(key, L"GpuCurvePts", curve_pts);
            /* Keep the legacy two-point values for older builds. */
            settings_write_dword(key, L"GpuTempLow", (DWORD)temps[0]);
            settings_write_dword(key, L"GpuDutyLow", (DWORD)duties[0]);
            settings_write_dword(key, L"GpuTempHigh",
                                 (DWORD)temps[point_count - 1]);
            settings_write_dword(key, L"GpuDutyHigh",
                                 (DWORD)duties[point_count - 1]);
        }
    }

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
    /* In tray mode app->hwnd is SW_HIDE and SetForegroundWindow may be
     * denied; activating the current foreground window first grants the
     * foreground right so the popup menu closes on a click-away. */
    HWND prev_foreground = GetForegroundWindow();
    if (prev_foreground) {
        SetForegroundWindow(prev_foreground);
    }
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

/* Live duty from the interactive curve graph (0-100 on both axes). */
static int nvidia_curve_duty(AppState *app)
{
    return clamp_int(ui_curve_value_at(app->curve_graph, app->gpu.temperature_c),
                     0, 100);
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

/* Slider position 0..100 <-> poll interval. 0 = slowest (UPDATE_MAX_MS),
 * 100 = fastest (UPDATE_MIN_MS). The interval is quantised to 100 ms so the
 * readout always lands on a clean tenth of a second. */
static int update_ms_from_pos(int pos)
{
    if (pos < 0) {
        pos = 0;
    }
    if (pos > 100) {
        pos = 100;
    }
    int raw = UPDATE_MAX_MS - (pos * (UPDATE_MAX_MS - UPDATE_MIN_MS)) / 100;
    raw = (raw + 50) / 100 * 100;
    if (raw < UPDATE_MIN_MS) {
        raw = UPDATE_MIN_MS;
    }
    if (raw > UPDATE_MAX_MS) {
        raw = UPDATE_MAX_MS;
    }
    return raw;
}

static int update_pos_from_ms(int ms)
{
    int pos = (UPDATE_MAX_MS - ms) * 100 / (UPDATE_MAX_MS - UPDATE_MIN_MS);
    if (pos < 0) {
        pos = 0;
    }
    if (pos > 100) {
        pos = 100;
    }
    return pos;
}

static void update_interval_label(AppState *app)
{
    wchar_t text[16];
    int ms = app->update_ms;
    swprintf(text, sizeof(text) / sizeof(text[0]), L"%d.%d s", ms / 1000,
             (ms % 1000) / 100);
    SetWindowTextW(app->update_readout, text);
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

static void update_gpu_stats_view(AppState *app)
{
    GpuStats *s = &app->gpu_stats;
    wchar_t text[32];

    for (int i = 0; i < STAT_ROW_COUNT; ++i) {
        switch (i) {
        case 0:
            if (s->have_clocks) {
                swprintf(text, sizeof(text) / sizeof(text[0]), L"%d MHz",
                         s->gpu_clock_mhz);
            } else {
                copy_wstr(text, sizeof(text) / sizeof(text[0]), L"—");
            }
            break;
        case 1:
            if (s->have_clocks) {
                swprintf(text, sizeof(text) / sizeof(text[0]), L"%d MHz",
                         s->mem_clock_mhz);
            } else {
                copy_wstr(text, sizeof(text) / sizeof(text[0]), L"—");
            }
            break;
        case 2:
            /* Live core voltage via the undocumented NVAPI GetCurrentVoltage
             * call (see nvidia_temp.c); NVML/nvidia-smi expose no voltage.
             * Falls back to a quiet placeholder when the GPU/driver does not
             * report it. */
            if (app->gpu.have_voltage) {
                swprintf(text, sizeof(text) / sizeof(text[0]), L"%d mV",
                         app->gpu.voltage_mv);
            } else {
                copy_wstr(text, sizeof(text) / sizeof(text[0]), L"—");
            }
            break;
        case 3:
            if (s->have_fan) {
                swprintf(text, sizeof(text) / sizeof(text[0]), L"%d %%",
                         s->fan_speed_pct);
            } else {
                copy_wstr(text, sizeof(text) / sizeof(text[0]), L"—");
            }
            break;
        case 4:
            if (s->have_power) {
                swprintf(text, sizeof(text) / sizeof(text[0]), L"%d W",
                         s->power_mw / 1000);
            } else {
                copy_wstr(text, sizeof(text) / sizeof(text[0]), L"—");
            }
            break;
        default:
            if (s->have_limit) {
                swprintf(text, sizeof(text) / sizeof(text[0]), L"%d W",
                         s->power_limit_mw / 1000);
            } else {
                copy_wstr(text, sizeof(text) / sizeof(text[0]), L"—");
            }
            break;
        }
        SetWindowTextW(app->stat_values[i], text);
    }

    int pct = -1;
    if (s->have_power && s->have_limit && s->power_limit_mw > 0) {
        pct = s->power_mw * 100 / s->power_limit_mw;
        if (pct > 100) {
            pct = 100;
        }
    }
    ui_meter_set(app->stats_meter, pct);
}

static void refresh_gpu_status(AppState *app)
{
    char err[256] = { 0 };
    app->gpu_ok = nvidia_temp_read(&app->gpu, err, sizeof(err));
    if (!app->gpu_ok && fake_device_enabled()) {
        app->gpu.available = true;
        app->gpu.gpu_count = 1;
        strcpy_s(app->gpu.name, sizeof(app->gpu.name), "GeForce RTX 4090 (Test)");
        double phase = (double)GetTickCount() / 25000.0;
        app->gpu.temperature_c = (int)(56.0 + 10.0 * sin(phase));
        app->gpu.voltage_mv = (int)(780.0 + 120.0 * sin(phase * 1.7));
        app->gpu.have_voltage = true;
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

    if (fake_device_enabled()) {
        /* Fake telemetry so the panel can be developed and screenshotted
         * without a GPU. */
        GpuStats *s = &app->gpu_stats;
        double phase = (double)GetTickCount() / 25000.0;
        memset(s, 0, sizeof(*s));
        s->available = true;
        s->have_clocks = s->have_fan = s->have_power = s->have_limit = true;
        s->gpu_clock_mhz = (int)(2200.0 + 500.0 * sin(phase * 1.3));
        s->mem_clock_mhz = 16801;
        s->fan_speed_pct = (int)(55.0 + 10.0 * sin(phase));
        s->power_mw = (int)(420000.0 + 80000.0 * sin(phase * 0.7));
        s->power_limit_mw = 720000;
    } else {
        gpu_stats_read(&app->gpu_stats);
    }
    update_gpu_stats_view(app);
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

    /* Populate device info before the fake check so is_fake_device() can see
     * the scanned path; corsair_open overwrites dev->info for real devices. */
    controller->device.info = app->devices[device_index];

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
    ++app->poll_counter;

    char first_error[256] = { 0 };
    int first_error_index = -1;
    for (int i = 0; i < app->device_count; ++i) {
        ControllerRuntime *controller = &app->controllers[i];
        bool newly_opened = false;
        bool controller_ok = true;
        char err[256] = { 0 };

        if (!is_fake_device(&controller->device)) {
            /* Healthy controllers are read every tick, at the same interval
             * as the GUI refresh. A controller whose last refresh failed is
             * only retried every REFRESH_FAIL_RETRY_EVERY-th tick so a dead
             * device does not trigger a close + reopen + full initialize on
             * every tick. */
            if (controller->refresh_failed &&
                app->poll_counter % REFRESH_FAIL_RETRY_EVERY != 0) {
                continue;
            }
        }

        if (!controller->opened) {
            if (!open_controller(app, i, err, sizeof(err))) {
                controller->refresh_failed = true;
                if (first_error_index < 0) {
                    first_error_index = i;
                    snprintf(first_error, sizeof(first_error), "%s", err);
                }
                continue;
            }
            newly_opened = true;
            controller->refresh_failed = false;
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
            controller->refresh_failed = false;
        } else if (!newly_opened &&
                   !corsair_refresh(&controller->device, err, sizeof(err))) {
            controller->refresh_failed = true;
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
    case WM_ERASEBKGND: {
        /* The class background brush is NULL, so the window erases with the
         * *current* theme brush; a handle stored in the class would go stale
         * when ui_set_dpi recreates the theme assets. */
        RECT client;
        GetClientRect(hwnd, &client);
        FillRect((HDC)wparam, &client, ui_theme()->ink_brush);
        return 1;
    }

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
    case WM_CTLCOLORBTN:
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
                                       ui_px(124), ui_px(240), ui_px(130), 0);

    app->curve_hint = make_child(
        hwnd, WC_STATICW, L"right-click add \u00b7 drag move", SS_LEFT,
        x_r + ui_px(12), ui_px(282), ui_px(240), ui_px(14), 0);
    ui_register_font_role(app->curve_hint, UI_FONT_CAPTION);
    ui_register_ctrl(app->curve_hint, UI_BG_PANEL, t->dim);
    app->curve_hint2 = make_child(
        hwnd, WC_STATICW, L"double-click remove", SS_LEFT, x_r + ui_px(12),
        ui_px(297), ui_px(240), ui_px(14), 0);
    ui_register_font_role(app->curve_hint2, UI_FONT_CAPTION);
    ui_register_ctrl(app->curve_hint2, UI_BG_PANEL, t->dim);

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

    /* Update (poll) interval, adjustable 0.5 s .. 5 s. Sits in the OPTIONS
     * panel; the footer stays on the window background. */
    app->update_label = make_child(hwnd, WC_STATICW, L"Update", SS_LEFT,
                                   x_r + ui_px(12), ui_px(540), ui_px(52), ui_px(16),
                                   0);
    ui_register_font_role(app->update_label, UI_FONT_CAPTION);
    ui_register_ctrl(app->update_label, UI_BG_PANEL, t->dim);
    app->update_slider = ui_make_control(hwnd, UI_CLASS_SLIDER, WS_TABSTOP,
                                         x_r + ui_px(72), ui_px(538), ui_px(116),
                                         ui_px(24), IDC_UPDATE_INTERVAL);
    app->update_readout = make_child(hwnd, WC_STATICW, L"5.0 s", SS_RIGHT,
                                     x_r + ui_px(196), ui_px(540), ui_px(56),
                                     ui_px(16), 0);
    ui_register_font_role(app->update_readout, UI_FONT_MONO_BOLD);
    ui_register_ctrl(app->update_readout, UI_BG_PANEL, t->text);

    /* Telemetry column ----------------------------------------------------- */
    int x_s = x_r + ui_px(264) + ui_px(12);
    app->stats_panel = ui_make_control(hwnd, UI_CLASS_PANEL, 0, x_s, ui_px(74),
                                       ui_px(224), ui_px(556), 0);
    app->stats_heading = make_child(hwnd, WC_STATICW, L"GPU TELEMETRY", SS_LEFT,
                                    x_s + ui_px(12), ui_px(84), ui_px(160), ui_px(16),
                                    0);
    ui_register_font_role(app->stats_heading, UI_FONT_HEADING);
    ui_register_ctrl(app->stats_heading, UI_BG_PANEL, t->dim);

    static const wchar_t *stat_names[STAT_ROW_COUNT] = {
        L"GPU Clock", L"Memory Clock", L"Voltage", L"Fan Speed",
        L"Board Power", L"Power Limit"
    };
    for (int i = 0; i < STAT_ROW_COUNT; ++i) {
        app->stat_labels[i] = make_child(hwnd, WC_STATICW, stat_names[i], SS_LEFT,
                                         x_s + ui_px(12), ui_px(110) + i * ui_px(30),
                                         ui_px(104), ui_px(16), 0);
        ui_register_font_role(app->stat_labels[i], UI_FONT_CAPTION);
        ui_register_ctrl(app->stat_labels[i], UI_BG_PANEL, t->dim);

        app->stat_values[i] = make_child(hwnd, WC_STATICW, L"—", SS_RIGHT,
                                         x_s + ui_px(120), ui_px(110) + i * ui_px(30),
                                         ui_px(92), ui_px(16), 0);
        ui_register_font_role(app->stat_values[i], UI_FONT_MONO);
        ui_register_ctrl(app->stat_values[i], UI_BG_PANEL, t->text);
    }

    /* Board-power utilisation meter (between Board Power and Power Limit). */
    app->stats_meter = ui_make_control(hwnd, UI_CLASS_METER, 0, x_s + ui_px(12),
                                       ui_px(262), ui_px(200), ui_px(10), 0);

    /* Footer ------------------------------------------------------------- */
    app->device_combo = ui_make_control(hwnd, UI_CLASS_COMBO, WS_TABSTOP, ui_px(14),
                                        ui_px(616), ui_px(210), ui_px(28),
                                        IDC_DEVICE_COMBO);
    ui_set_corner_bg(app->device_combo, t->ink);
    app->scan_btn = ui_make_control(hwnd, UI_CLASS_BUTTON, WS_TABSTOP, ui_px(234),
                                    ui_px(616), ui_px(70), ui_px(28), IDC_SCAN);
    SetWindowTextW(app->scan_btn, L"Scan");
    ui_set_corner_bg(app->scan_btn, t->ink);
    app->refresh_btn = ui_make_control(hwnd, UI_CLASS_BUTTON, WS_TABSTOP, ui_px(314),
                                       ui_px(616), ui_px(80), ui_px(28), IDC_REFRESH);
    SetWindowTextW(app->refresh_btn, L"Refresh");
    ui_set_corner_bg(app->refresh_btn, t->ink);
    app->apply_all_btn = ui_make_control(hwnd, UI_CLASS_BUTTON, WS_TABSTOP, ui_px(404),
                                         ui_px(616), ui_px(84), ui_px(28),
                                         IDC_APPLY_ALL);
    SetWindowTextW(app->apply_all_btn, L"Apply all");
    ui_set_corner_bg(app->apply_all_btn, t->ink);
    app->overview_btn = ui_make_control(hwnd, UI_CLASS_BUTTON, WS_TABSTOP, ui_px(498),
                                        ui_px(616), ui_px(84), ui_px(28), IDC_OVERVIEW);
    SetWindowTextW(app->overview_btn, L"Overview");
    ui_set_corner_bg(app->overview_btn, t->ink);

    app->status_label = make_child(hwnd, WC_STATICW, L"Scan for devices.", SS_LEFT,
                                   ui_px(14), ui_px(650), ui_px(700), ui_px(16),
                                   IDC_STATUS);
    ui_register_font_role(app->status_label, UI_FONT_CAPTION);
    ui_register_ctrl(app->status_label, UI_BG_INK, t->dim);

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
    SetWindowPos(app->stats_panel, HWND_BOTTOM, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE);

    load_global_settings(app);
    app->update_ms = clamp_int(app->update_ms, UPDATE_MIN_MS, UPDATE_MAX_MS);
    ui_slider_set(app->update_slider, update_pos_from_ms(app->update_ms));
    update_interval_label(app);
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
    int stats_w = ui_px(224);
    int left_w = w - m * 2 - right_w - stats_w - gap * 2;
    int x_r = m + left_w + gap;
    int x_s = x_r + right_w + gap;

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
    int ctrl_h = ui_px(150);
    /* The OPTIONS panel is sized to its content (heading + checkbox +
     * interval slider + save button); the GPU curve absorbs the remaining
     * height so the interval control always fits without being crammed. */
    int opts_h = ui_px(134);
    int curve_h = body_h - opts_h - ctrl_h - gap * 2;
    if (curve_h < ui_px(200)) {
        curve_h = ui_px(200);
    }

    MoveWindow(app->curve_panel, x_r, body_top, right_w, curve_h, TRUE);
    MoveWindow(app->curve_heading, x_r + ui_px(12), body_top + ui_px(10), ui_px(160),
               ui_px(16), TRUE);
    MoveWindow(app->gpu_status_label, x_r + ui_px(12), body_top + ui_px(30),
               right_w - ui_px(24), ui_px(16), TRUE);
    MoveWindow(app->curve_graph, x_r + ui_px(12), body_top + ui_px(50),
               right_w - ui_px(24), curve_h - ui_px(100), TRUE);
    MoveWindow(app->curve_hint, x_r + ui_px(12), body_top + curve_h - ui_px(46),
               right_w - ui_px(24), ui_px(14), TRUE);
    MoveWindow(app->curve_hint2, x_r + ui_px(12), body_top + curve_h - ui_px(31),
               right_w - ui_px(24), ui_px(14), TRUE);

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
    MoveWindow(app->autostart_checkbox, x_r + ui_px(12), options_top + ui_px(32),
               ui_px(180), ui_px(20), TRUE);
    MoveWindow(app->update_label, x_r + ui_px(12), options_top + ui_px(64), ui_px(52),
               ui_px(16), TRUE);
    MoveWindow(app->update_slider, x_r + ui_px(72), options_top + ui_px(62), ui_px(116),
               ui_px(24), TRUE);
    MoveWindow(app->update_readout, x_r + right_w - ui_px(68), options_top + ui_px(64),
               ui_px(56), ui_px(16), TRUE);
    MoveWindow(app->save_btn, x_r + ui_px(12), options_top + opts_h - ui_px(38),
               right_w - ui_px(24), ui_px(28), TRUE);

    /* Telemetry column. Six readout rows plus the power meter are spread
     * evenly across the body height (seven slots); the meter owns slot 5,
     * so the Power Limit row is the last one. */
    MoveWindow(app->stats_panel, x_s, body_top, stats_w, body_h, TRUE);
    MoveWindow(app->stats_heading, x_s + ui_px(12), body_top + ui_px(10),
               ui_px(160), ui_px(16), TRUE);
    int stat_slots = 7;
    int stat_area_top = body_top + ui_px(52);
    int stat_area_bottom = body_bottom - ui_px(12);
    int stat_pitch = (stat_area_bottom - stat_area_top) / stat_slots;
    if (stat_pitch < ui_px(22)) {
        stat_pitch = ui_px(22);
    }
    for (int i = 0; i < STAT_ROW_COUNT; ++i) {
        int slot = (i < 5) ? i : 6;
        int y = stat_area_top + slot * stat_pitch + (stat_pitch - ui_px(16)) / 2;
        MoveWindow(app->stat_labels[i], x_s + ui_px(12), y, ui_px(104), ui_px(16),
                   TRUE);
        MoveWindow(app->stat_values[i], x_s + stats_w - ui_px(12) - ui_px(92), y,
                   ui_px(92), ui_px(16), TRUE);
    }
    int meter_y =
        stat_area_top + 5 * stat_pitch + (stat_pitch - ui_px(10)) / 2;
    MoveWindow(app->stats_meter, x_s + ui_px(12), meter_y, stats_w - ui_px(24),
               ui_px(10), TRUE);

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
    case WM_ERASEBKGND: {
        /* The class background brush is NULL, so the window erases with the
         * *current* theme brush; a handle stored in the class would go stale
         * when ui_set_dpi recreates the theme assets. */
        RECT client;
        GetClientRect(hwnd, &client);
        FillRect((HDC)wparam, &client, ui_theme()->ink_brush);
        return 1;
    }

    case WM_CREATE:
        app->hwnd = hwnd;
        create_main_controls(app, hwnd);
        RECT client;
        GetClientRect(hwnd, &client);
        layout_main(app);
        scan_devices(app);
        SetTimer(hwnd, REFRESH_TIMER, app->update_ms, NULL);
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
        }
        return 0;
    }

    case UI_MSG_CURVE_CHANGED:
        save_settings(app);
        update_overview_view(app);
        set_status(app, L"Curve updated.");
        return 0;

    case WM_HSCROLL:
        if ((HWND)lparam == app->update_slider) {
            app->update_ms = update_ms_from_pos(ui_slider_value(app->update_slider));
            update_interval_label(app);
            SetTimer(hwnd, REFRESH_TIMER, app->update_ms, NULL);
            if (LOWORD(wparam) == TB_ENDTRACK || LOWORD(wparam) == SB_ENDSCROLL) {
                save_global_settings(app);
            }
            return 0;
        }
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
        info->ptMinTrackSize.x = ui_px(1040);
        info->ptMinTrackSize.y = ui_px(690);
        return 0;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
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
        gpu_stats_shutdown();
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

    INITCOMMONCONTROLSEX icc;
    WNDCLASSW wc;
    WNDCLASSW overview_wc;
    HWND hwnd;
    MSG msg;

    ZeroMemory(&g_app, sizeof(g_app));
    g_app.active_device_index = -1;
    g_app.taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");
    /* Exact token match on the wide command line; a substring match on the
     * ANSI string would also trigger tray mode for path arguments that
     * merely contain "--tray". */
    (void)cmd_line;
    g_app.start_in_tray = false;
    {
        int argc = 0;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 0; i < argc; ++i) {
                if (lstrcmpW(argv[i], L"--tray") == 0) {
                    g_app.start_in_tray = true;
                    break;
                }
            }
            LocalFree(argv);
        }
    }

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"CorsairNvidiaFanControlWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    /* Background erasing is handled in wnd_proc/overview_wnd_proc
     * (WM_ERASEBKGND) with the current theme brush; a brush stored in the
     * class goes stale after ui_set_dpi recreates the theme assets. */
    wc.hbrBackground = NULL;

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
                           CW_USEDEFAULT, CW_USEDEFAULT, 1128, 720, NULL, NULL, instance,
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