#include "gpu_stats.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define NVML_SUCCESS 0
#define NVML_ERROR_UNINITIALIZED 1
#define NVML_ERROR_NOT_SUPPORTED 3
#define NVML_ERROR_NO_PERMISSION 4
#define NVML_ERROR_NOT_FOUND 6
#define NVML_ERROR_DRIVER_NOT_LOADED 9
#define NVML_ERROR_TIMEOUT 10

#define NVML_CLOCK_GRAPHICS 0
#define NVML_CLOCK_MEM 2
#define NVML_CLOCK_ID_CURRENT 0

typedef int nvmlReturn;
typedef void *nvmlDevice_t;

typedef nvmlReturn(*NvmlInitFn)(void);
typedef nvmlReturn(*NvmlShutdownFn)(void);
typedef nvmlReturn(*NvmlDeviceGetHandleByIndexFn)(unsigned int index, nvmlDevice_t *device);
typedef nvmlReturn(*NvmlDeviceGetClockFn)(nvmlDevice_t device, unsigned int clock_type,
                                          unsigned int clock_id, unsigned int *clock_mhz);
typedef nvmlReturn(*NvmlDeviceGetFanSpeedFn)(nvmlDevice_t device, unsigned int *speed);
typedef nvmlReturn(*NvmlDeviceGetPowerUsageFn)(nvmlDevice_t device, unsigned int *power_mw);
typedef nvmlReturn(*NvmlDeviceGetPowerLimitFn)(nvmlDevice_t device, unsigned int *limit_mw);
typedef nvmlReturn(*NvmlDeviceGetThrottleStatusFn)(nvmlDevice_t device,
                                                    unsigned long long *status);

static void set_error(char *err, size_t err_len, const char *fmt, ...)
{
    if (!err || err_len == 0) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(err, err_len, fmt, args);
    va_end(args);
    err[err_len - 1] = '\0';
}

/* The NVML session is created once per process and kept until a driver-level
 * error forces a fresh re-init (same lifecycle pattern as the NVAPI session
 * in nvidia_temp.c). */
static struct {
    HMODULE dll;
    NvmlInitFn init;
    NvmlShutdownFn shutdown;
    NvmlDeviceGetHandleByIndexFn get_handle;
    NvmlDeviceGetClockFn get_clock;
    NvmlDeviceGetFanSpeedFn get_fan;
    NvmlDeviceGetPowerUsageFn get_power;
    NvmlDeviceGetPowerLimitFn get_power_limit;
    NvmlDeviceGetThrottleStatusFn get_throttle;
    nvmlDevice_t device;
    bool ready;
} g_nvml;

static void nvml_release(void)
{
    if (g_nvml.shutdown) {
        g_nvml.shutdown();
    }
    if (g_nvml.dll) {
        FreeLibrary(g_nvml.dll);
    }
    memset(&g_nvml, 0, sizeof(g_nvml));
}

static bool nvml_ensure(char *err, size_t err_len)
{
    if (g_nvml.ready) {
        return true;
    }

    HMODULE dll = LoadLibraryW(L"nvml.dll");
    if (!dll) {
        set_error(err, err_len, "nvml.dll not found. Install or update the NVIDIA driver.");
        return false;
    }
    g_nvml.dll = dll;

    g_nvml.init = (NvmlInitFn)GetProcAddress(dll, "nvmlInit_v2");
    g_nvml.shutdown = (NvmlShutdownFn)GetProcAddress(dll, "nvmlShutdown");
    g_nvml.get_handle = (NvmlDeviceGetHandleByIndexFn)GetProcAddress(
        dll, "nvmlDeviceGetHandleByIndex_v2");
    g_nvml.get_clock = (NvmlDeviceGetClockFn)GetProcAddress(dll, "nvmlDeviceGetClock");
    g_nvml.get_fan = (NvmlDeviceGetFanSpeedFn)GetProcAddress(dll, "nvmlDeviceGetFanSpeed");
    g_nvml.get_power = (NvmlDeviceGetPowerUsageFn)GetProcAddress(dll, "nvmlDeviceGetPowerUsage");
    g_nvml.get_power_limit = (NvmlDeviceGetPowerLimitFn)GetProcAddress(
        dll, "nvmlDeviceGetPowerManagementLimit");
    g_nvml.get_throttle = (NvmlDeviceGetThrottleStatusFn)GetProcAddress(
        dll, "nvmlDeviceGetCurrentClocksThrottleReasons");

    if (!g_nvml.init || !g_nvml.shutdown || !g_nvml.get_handle) {
        set_error(err, err_len, "Required NVML functions are missing from nvml.dll.");
        nvml_release();
        return false;
    }

    if (g_nvml.init() != NVML_SUCCESS) {
        set_error(err, err_len, "nvmlInit failed.");
        nvml_release();
        return false;
    }

    if (g_nvml.get_handle(0, &g_nvml.device) != NVML_SUCCESS || !g_nvml.device) {
        set_error(err, err_len, "No NVIDIA GPU detected.");
        nvml_release();
        return false;
    }

    g_nvml.ready = true;
    return true;
}

bool gpu_stats_read(GpuStats *stats)
{
    char err[128] = { 0 };

    if (!stats) {
        return false;
    }
    memset(stats, 0, sizeof(*stats));

    if (!nvml_ensure(err, sizeof(err))) {
        set_error(stats->err, sizeof(stats->err), "%s", err);
        return false;
    }

    unsigned int value = 0;
    nvmlReturn session_rc = NVML_SUCCESS;

    stats->available = true;

    if (g_nvml.get_clock) {
        session_rc = g_nvml.get_clock(g_nvml.device, NVML_CLOCK_GRAPHICS, NVML_CLOCK_ID_CURRENT,
                                      &value);
        if (session_rc == NVML_SUCCESS) {
            stats->gpu_clock_mhz = (int)value;
            stats->have_clocks = true;
        }
    }
    if (stats->have_clocks && g_nvml.get_clock) {
        session_rc = g_nvml.get_clock(g_nvml.device, NVML_CLOCK_MEM, NVML_CLOCK_ID_CURRENT,
                                      &value);
        if (session_rc == NVML_SUCCESS) {
            stats->mem_clock_mhz = (int)value;
        }
    }
    if (g_nvml.get_fan) {
        session_rc = g_nvml.get_fan(g_nvml.device, &value);
        if (session_rc == NVML_SUCCESS) {
            stats->fan_speed_pct = (int)(value > 100 ? 100 : value);
            stats->have_fan = true;
        }
    }
    if (g_nvml.get_power) {
        session_rc = g_nvml.get_power(g_nvml.device, &value);
        if (session_rc == NVML_SUCCESS) {
            stats->power_mw = (int)value;
            stats->have_power = true;
        }
    }
    if (g_nvml.get_power_limit) {
        session_rc = g_nvml.get_power_limit(g_nvml.device, &value);
        if (session_rc == NVML_SUCCESS) {
            stats->power_limit_mw = (int)value;
            stats->have_limit = true;
        }
    }
    if (g_nvml.get_throttle) {
        unsigned long long status = 0;
        session_rc = g_nvml.get_throttle(g_nvml.device, &status);
        if (session_rc == NVML_SUCCESS) {
            stats->throttle_status = status;
            stats->have_throttle = true;
        }
    }

    /* Transient session errors (driver unload, timeout): release the session
     * so the next tick retries with a fresh init. Per-query errors like
     * NOT_SUPPORTED are persistent; keep the session to avoid churn. */
    if (session_rc == NVML_ERROR_UNINITIALIZED || session_rc == NVML_ERROR_TIMEOUT ||
        session_rc == NVML_ERROR_DRIVER_NOT_LOADED || session_rc == NVML_ERROR_NOT_FOUND) {
        nvml_release();
    }

    return true;
}

void gpu_stats_shutdown(void)
{
    nvml_release();
}