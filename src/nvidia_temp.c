#include "nvidia_temp.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define NVAPI_OK 0
#define NVAPI_MAX_PHYSICAL_GPUS 64
#define NVAPI_SHORT_STRING_MAX 64
#define NVAPI_MAX_THERMAL_SENSORS_PER_GPU 3
#define NVAPI_THERMAL_TARGET_GPU 1
#define NVAPI_THERMAL_TARGET_ALL 15

#define NVAPI_ID_INITIALIZE 0x0150e828
#define NVAPI_ID_UNLOAD 0xd22bdd7e
#define NVAPI_ID_GET_ERROR_MESSAGE 0x6c2d048c
#define NVAPI_ID_ENUM_PHYSICAL_GPUS 0xe5ac921f
#define NVAPI_ID_GPU_GET_FULL_NAME 0xceee8e9f
#define NVAPI_ID_GPU_GET_THERMAL_SETTINGS 0xe3640a56

#define MAKE_NVAPI_VERSION(type_name, version) ((unsigned int)(sizeof(type_name) | ((version) << 16)))

typedef int NvAPI_Status;
typedef unsigned int NvU32;
typedef int NvS32;
typedef void *NvPhysicalGpuHandle;
typedef char NvAPI_ShortString[NVAPI_SHORT_STRING_MAX];

typedef enum NvThermalController {
    NVAPI_THERMAL_CONTROLLER_NONE = 0,
    NVAPI_THERMAL_CONTROLLER_GPU_INTERNAL = 1,
    NVAPI_THERMAL_CONTROLLER_UNKNOWN = -1
} NvThermalController;

typedef enum NvThermalTarget {
    NVAPI_THERMAL_TARGET_NONE = 0,
    NVAPI_THERMAL_TARGET_MEMORY = 2,
    NVAPI_THERMAL_TARGET_POWER_SUPPLY = 4,
    NVAPI_THERMAL_TARGET_BOARD = 8,
    NVAPI_THERMAL_TARGET_UNKNOWN = -1
} NvThermalTarget;

typedef struct NvGpuThermalSettings {
    NvU32 version;
    NvU32 count;
    struct {
        NvThermalController controller;
        NvS32 default_min_temp;
        NvS32 default_max_temp;
        NvS32 current_temp;
        NvThermalTarget target;
    } sensor[NVAPI_MAX_THERMAL_SENSORS_PER_GPU];
} NvGpuThermalSettings;

typedef void *(__cdecl *NvAPI_QueryInterfaceFn)(unsigned int id);
typedef NvAPI_Status(__cdecl *NvAPI_InitializeFn)(void);
typedef NvAPI_Status(__cdecl *NvAPI_UnloadFn)(void);
typedef NvAPI_Status(__cdecl *NvAPI_GetErrorMessageFn)(NvAPI_Status status, NvAPI_ShortString text);
typedef NvAPI_Status(__cdecl *NvAPI_EnumPhysicalGPUsFn)(NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS],
                                                         NvU32 *count);
typedef NvAPI_Status(__cdecl *NvAPI_GPU_GetFullNameFn)(NvPhysicalGpuHandle handle, NvAPI_ShortString name);
typedef NvAPI_Status(__cdecl *NvAPI_GPU_GetThermalSettingsFn)(NvPhysicalGpuHandle handle, NvU32 sensor_index,
                                                               NvGpuThermalSettings *settings);

typedef struct NvApi {
    HMODULE dll;
    NvAPI_QueryInterfaceFn query_interface;
    NvAPI_InitializeFn initialize;
    NvAPI_UnloadFn unload;
    NvAPI_GetErrorMessageFn get_error_message;
    NvAPI_EnumPhysicalGPUsFn enum_physical_gpus;
    NvAPI_GPU_GetFullNameFn gpu_get_full_name;
    NvAPI_GPU_GetThermalSettingsFn gpu_get_thermal_settings;
} NvApi;

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

static void nvapi_error(NvApi *api, NvAPI_Status status, char *err, size_t err_len, const char *prefix)
{
    NvAPI_ShortString text = { 0 };

    if (api && api->get_error_message && api->get_error_message(status, text) == NVAPI_OK && text[0] != '\0') {
        set_error(err, err_len, "%s: %s", prefix, text);
    } else {
        set_error(err, err_len, "%s: NVAPI status %d", prefix, status);
    }
}

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t i = 0;
    for (; i + 1 < dst_len && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void *load_fn(NvApi *api, unsigned int id)
{
    return api->query_interface ? api->query_interface(id) : NULL;
}

static HMODULE load_nvapi64(void)
{
    HMODULE dll = LoadLibraryExW(L"nvapi64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dll) {
        return dll;
    }

    wchar_t path[MAX_PATH];
    UINT len = GetSystemDirectoryW(path, (UINT)(sizeof(path) / sizeof(path[0])));
    const wchar_t suffix[] = L"\\nvapi64.dll";
    size_t suffix_len = sizeof(suffix) / sizeof(suffix[0]);

    if (len == 0 || len > (sizeof(path) / sizeof(path[0])) - suffix_len) {
        return NULL;
    }

    for (size_t i = 0; i < suffix_len; ++i) {
        path[len + i] = suffix[i];
    }
    return LoadLibraryW(path);
}

static bool nvapi_load(NvApi *api, char *err, size_t err_len)
{
    NvAPI_Status status;

    memset(api, 0, sizeof(*api));
    api->dll = load_nvapi64();
    if (!api->dll) {
        set_error(err, err_len, "nvapi64.dll not found. Install or update the NVIDIA driver.");
        return false;
    }

    api->query_interface = (NvAPI_QueryInterfaceFn)GetProcAddress(api->dll, "nvapi_QueryInterface");
    if (!api->query_interface) {
        set_error(err, err_len, "nvapi_QueryInterface not exported by nvapi64.dll.");
        FreeLibrary(api->dll);
        memset(api, 0, sizeof(*api));
        return false;
    }

    api->initialize = (NvAPI_InitializeFn)load_fn(api, NVAPI_ID_INITIALIZE);
    api->unload = (NvAPI_UnloadFn)load_fn(api, NVAPI_ID_UNLOAD);
    api->get_error_message = (NvAPI_GetErrorMessageFn)load_fn(api, NVAPI_ID_GET_ERROR_MESSAGE);
    api->enum_physical_gpus = (NvAPI_EnumPhysicalGPUsFn)load_fn(api, NVAPI_ID_ENUM_PHYSICAL_GPUS);
    api->gpu_get_full_name = (NvAPI_GPU_GetFullNameFn)load_fn(api, NVAPI_ID_GPU_GET_FULL_NAME);
    api->gpu_get_thermal_settings =
        (NvAPI_GPU_GetThermalSettingsFn)load_fn(api, NVAPI_ID_GPU_GET_THERMAL_SETTINGS);

    if (!api->initialize || !api->enum_physical_gpus || !api->gpu_get_thermal_settings) {
        set_error(err, err_len, "Required NVAPI functions are missing.");
        FreeLibrary(api->dll);
        memset(api, 0, sizeof(*api));
        return false;
    }

    status = api->initialize();
    if (status != NVAPI_OK) {
        nvapi_error(api, status, err, err_len, "NvAPI_Initialize");
        FreeLibrary(api->dll);
        memset(api, 0, sizeof(*api));
        return false;
    }

    return true;
}

static void nvapi_unload(NvApi *api)
{
    if (!api) {
        return;
    }
    if (api->unload) {
        api->unload();
    }
    if (api->dll) {
        FreeLibrary(api->dll);
    }
    memset(api, 0, sizeof(*api));
}

static bool valid_temp(int temp)
{
    return temp >= -20 && temp <= 125;
}

bool nvidia_temp_read(NvidiaGpuStatus *status, char *err, size_t err_len)
{
    NvApi api;
    NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS] = { 0 };
    NvGpuThermalSettings thermal;
    NvU32 count = 0;
    NvAPI_Status nvstatus;
    bool ok = false;

    if (!status) {
        set_error(err, err_len, "Invalid NVIDIA status buffer.");
        return false;
    }

    memset(status, 0, sizeof(*status));

    if (!nvapi_load(&api, err, err_len)) {
        return false;
    }

    nvstatus = api.enum_physical_gpus(handles, &count);
    if (nvstatus != NVAPI_OK || count == 0) {
        nvapi_error(&api, nvstatus, err, err_len, "NvAPI_EnumPhysicalGPUs");
        goto out;
    }

    status->gpu_count = (int)count;
    if (api.gpu_get_full_name) {
        NvAPI_ShortString name = { 0 };
        if (api.gpu_get_full_name(handles[0], name) == NVAPI_OK && name[0] != '\0') {
            copy_str(status->name, sizeof(status->name), name);
        }
    }
    if (status->name[0] == '\0') {
        copy_str(status->name, sizeof(status->name), "NVIDIA GPU");
    }

    memset(&thermal, 0, sizeof(thermal));
    thermal.version = MAKE_NVAPI_VERSION(NvGpuThermalSettings, 2);

    nvstatus = api.gpu_get_thermal_settings(handles[0], NVAPI_THERMAL_TARGET_ALL, &thermal);
    if (nvstatus != NVAPI_OK) {
        nvapi_error(&api, nvstatus, err, err_len, "NvAPI_GPU_GetThermalSettings");
        goto out;
    }

    for (NvU32 i = 0; i < thermal.count && i < NVAPI_MAX_THERMAL_SENSORS_PER_GPU; ++i) {
        if (thermal.sensor[i].target == NVAPI_THERMAL_TARGET_GPU &&
            valid_temp(thermal.sensor[i].current_temp)) {
            status->temperature_c = thermal.sensor[i].current_temp;
            status->available = true;
            ok = true;
            goto out;
        }
    }

    for (NvU32 i = 0; i < thermal.count && i < NVAPI_MAX_THERMAL_SENSORS_PER_GPU; ++i) {
        if (valid_temp(thermal.sensor[i].current_temp)) {
            status->temperature_c = thermal.sensor[i].current_temp;
            status->available = true;
            ok = true;
            goto out;
        }
    }

    set_error(err, err_len, "No valid NVIDIA GPU temperature sensor returned.");

out:
    nvapi_unload(&api);
    return ok;
}
