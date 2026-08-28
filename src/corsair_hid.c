#include "corsair_hid.h"

#include <hidsdi.h>
#include <setupapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define CORSAIR_VID 0x1b1c
#define CORSAIR_PID_COMMANDER_PRO 0x0c10
#define CORSAIR_PID_1000D 0x1d00

#define CMD_GET_FIRMWARE 0x02
#define CMD_GET_BOOTLOADER 0x06
#define CMD_GET_TEMP_CONFIG 0x10
#define CMD_GET_TEMP 0x11
#define CMD_GET_VOLTS 0x12
#define CMD_GET_FAN_MODES 0x20
#define CMD_GET_FAN_RPM 0x21
#define CMD_SET_FAN_DUTY 0x23
#define CMD_SET_FAN_MODE 0x28

#define RESPONSE_LEN 16
#define IO_TIMEOUT_MS 300
#define MUTEX_TIMEOUT_MS 2000
/* Read-only sensor refreshes fail fast when another application (iCUE) holds
 * the device guard, so the UI thread is not blocked for seconds. Writes and
 * initialization keep the full MUTEX_TIMEOUT_MS budget. */
#define MUTEX_READ_TIMEOUT_MS 500

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

static const wchar_t *model_for_pid(uint16_t pid)
{
    switch (pid) {
    case CORSAIR_PID_COMMANDER_PRO:
        return L"Corsair Commander Pro";
    case CORSAIR_PID_1000D:
        return L"Corsair Obsidian 1000D";
    default:
        return L"Unsupported Corsair device";
    }
}

static bool is_supported(uint16_t vid, uint16_t pid)
{
    return vid == CORSAIR_VID &&
           (pid == CORSAIR_PID_COMMANDER_PRO || pid == CORSAIR_PID_1000D);
}

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

static uint16_t u16_be(const unsigned char *buf, int offset)
{
    return (uint16_t)(((uint16_t)buf[offset] << 8) | (uint16_t)buf[offset + 1]);
}

const wchar_t *corsair_fan_mode_name(int mode)
{
    switch (mode) {
    case CORSAIR_FAN_DC:
        return L"DC";
    case CORSAIR_FAN_PWM:
        return L"PWM";
    case CORSAIR_FAN_DISCONNECTED:
        return L"N/A";
    default:
        return L"Other";
    }
}

int corsair_find_devices(CorsairDeviceInfo *devices, int max_devices, char *err, size_t err_len)
{
    GUID hid_guid;
    HDEVINFO dev_info;
    SP_DEVICE_INTERFACE_DATA if_data;
    int count = 0;

    if (!devices || max_devices <= 0) {
        set_error(err, err_len, "Invalid device buffer.");
        return 0;
    }

    HidD_GetHidGuid(&hid_guid);
    dev_info = SetupDiGetClassDevsW(&hid_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE) {
        set_error(err, err_len, "SetupDiGetClassDevs failed: %lu", GetLastError());
        return 0;
    }

    ZeroMemory(&if_data, sizeof(if_data));
    if_data.cbSize = sizeof(if_data);

    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(dev_info, NULL, &hid_guid, index, &if_data); ++index) {
        DWORD required = 0;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = NULL;
        HANDLE handle = INVALID_HANDLE_VALUE;
        HIDD_ATTRIBUTES attrs;

        SetupDiGetDeviceInterfaceDetailW(dev_info, &if_data, NULL, 0, &required, NULL);
        if (required == 0) {
            continue;
        }

        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, required);
        if (!detail) {
            continue;
        }
        detail->cbSize = sizeof(*detail);

        if (!SetupDiGetDeviceInterfaceDetailW(dev_info, &if_data, detail, required, NULL, NULL)) {
            HeapFree(GetProcessHeap(), 0, detail);
            continue;
        }

        handle = CreateFileW(detail->DevicePath, 0,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            HeapFree(GetProcessHeap(), 0, detail);
            continue;
        }

        ZeroMemory(&attrs, sizeof(attrs));
        attrs.Size = sizeof(attrs);
        if (HidD_GetAttributes(handle, &attrs) &&
            is_supported(attrs.VendorID, attrs.ProductID) &&
            count < max_devices) {
            CorsairDeviceInfo *dst = &devices[count++];
            ZeroMemory(dst, sizeof(*dst));
            copy_wstr(dst->path, sizeof(dst->path) / sizeof(dst->path[0]), detail->DevicePath);
            dst->vendor_id = attrs.VendorID;
            dst->product_id = attrs.ProductID;
            copy_wstr(dst->model, sizeof(dst->model) / sizeof(dst->model[0]), model_for_pid(attrs.ProductID));

            if (!HidD_GetProductString(handle, dst->product, sizeof(dst->product)) || dst->product[0] == L'\0') {
                copy_wstr(dst->product, sizeof(dst->product) / sizeof(dst->product[0]), dst->model);
            }
        }

        CloseHandle(handle);
        HeapFree(GetProcessHeap(), 0, detail);
    }

    SetupDiDestroyDeviceInfoList(dev_info);

    if (count == 0) {
        set_error(err, err_len, "No supported Corsair Commander Pro / 1000D HID device found.");
    }
    return count;
}

static bool wait_io(HANDLE handle, OVERLAPPED *ov, DWORD timeout_ms, DWORD *bytes, char *err, size_t err_len)
{
    DWORD wait = WaitForSingleObject(ov->hEvent, timeout_ms);
    if (wait == WAIT_TIMEOUT) {
        CancelIoEx(handle, ov);
        GetOverlappedResult(handle, ov, bytes, TRUE);
        set_error(err, err_len, "HID transaction timed out.");
        return false;
    }
    if (wait != WAIT_OBJECT_0) {
        set_error(err, err_len, "WaitForSingleObject failed: %lu", GetLastError());
        return false;
    }
    if (!GetOverlappedResult(handle, ov, bytes, FALSE)) {
        set_error(err, err_len, "HID overlapped result failed: %lu", GetLastError());
        return false;
    }
    return true;
}

static bool write_report(CorsairDevice *dev, const unsigned char *buf, DWORD len, char *err, size_t err_len)
{
    DWORD bytes = 0;
    ResetEvent(dev->write_ov.hEvent);

    if (WriteFile(dev->handle, buf, len, &bytes, &dev->write_ov)) {
        return bytes == len;
    }

    if (GetLastError() != ERROR_IO_PENDING) {
        set_error(err, err_len, "WriteFile failed: %lu", GetLastError());
        return false;
    }

    if (!wait_io(dev->handle, &dev->write_ov, IO_TIMEOUT_MS, &bytes, err, err_len)) {
        return false;
    }
    if (bytes != len) {
        set_error(err, err_len, "Short HID write: %lu of %lu bytes.", bytes, len);
        return false;
    }
    return true;
}

static bool read_report(CorsairDevice *dev, unsigned char *buf, DWORD len, DWORD timeout_ms,
                        DWORD *bytes_read, char *err, size_t err_len)
{
    DWORD bytes = 0;
    ResetEvent(dev->read_ov.hEvent);

    if (ReadFile(dev->handle, buf, len, &bytes, &dev->read_ov)) {
        if (bytes_read) {
            *bytes_read = bytes;
        }
        return true;
    }

    if (GetLastError() != ERROR_IO_PENDING) {
        set_error(err, err_len, "ReadFile failed: %lu", GetLastError());
        return false;
    }

    if (!wait_io(dev->handle, &dev->read_ov, timeout_ms, &bytes, err, err_len)) {
        return false;
    }
    if (bytes_read) {
        *bytes_read = bytes;
    }
    return true;
}

static void drain_reports(CorsairDevice *dev)
{
    unsigned char tmp[128];
    DWORD got = 0;
    char ignored[64];

    for (int i = 0; i < 8; ++i) {
        if (!read_report(dev, tmp, dev->input_report_len, 1, &got, ignored, sizeof(ignored))) {
            break;
        }
        if (got == 0) {
            break;
        }
    }
}

static bool response_success(unsigned char code, char *err, size_t err_len)
{
    switch (code) {
    case 0x00:
        return true;
    case 0x01:
        set_error(err, err_len, "Device rejected command: invalid command.");
        return false;
    case 0x10:
        set_error(err, err_len, "Device rejected command: invalid argument.");
        return false;
    case 0x11:
        set_error(err, err_len, "Device reported no data for this channel.");
        return false;
    case 0x12:
        set_error(err, err_len, "Device reported this channel is not fixed-duty controlled.");
        return false;
    default:
        set_error(err, err_len, "Unknown device response: 0x%02x.", code);
        return false;
    }
}

static bool send_command(CorsairDevice *dev, unsigned char command,
                         const unsigned char *data, size_t data_len,
                         unsigned char response[RESPONSE_LEN],
                         uint32_t mutex_timeout_ms,
                         char *err, size_t err_len)
{
    unsigned char out[128];
    unsigned char in[128];
    DWORD bytes_read = 0;
    DWORD out_len = dev->output_report_len;
    DWORD in_len = dev->input_report_len;
    DWORD wait_result;
    bool ok = false;

    if (out_len < 3 || out_len > sizeof(out) || in_len < RESPONSE_LEN || in_len > sizeof(in)) {
        set_error(err, err_len, "Unsupported HID report sizes: out=%u in=%u.",
                  (unsigned)out_len, (unsigned)in_len);
        return false;
    }
    if (data_len > out_len - 2) {
        data_len = out_len - 2;
    }

    wait_result = WaitForSingleObject(dev->io_mutex, mutex_timeout_ms);
    if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
        set_error(err, err_len, "Could not acquire Corsair HID mutex.");
        return false;
    }

    ZeroMemory(out, sizeof(out));
    out[0] = 0x00;
    out[1] = command;
    if (data && data_len > 0) {
        memcpy(&out[2], data, data_len);
    }

    drain_reports(dev);

    if (!write_report(dev, out, out_len, err, err_len)) {
        goto out_unlock;
    }

    ZeroMemory(in, sizeof(in));
    if (!read_report(dev, in, in_len, IO_TIMEOUT_MS, &bytes_read, err, err_len)) {
        goto out_unlock;
    }

    if (bytes_read > RESPONSE_LEN) {
        memcpy(response, &in[1], RESPONSE_LEN);
    } else if (bytes_read == RESPONSE_LEN) {
        memcpy(response, in, RESPONSE_LEN);
    } else {
        set_error(err, err_len, "Short HID response: %lu bytes.", bytes_read);
        goto out_unlock;
    }

    ok = response_success(response[0], err, err_len);

out_unlock:
    ReleaseMutex(dev->io_mutex);
    return ok;
}

bool corsair_open(CorsairDevice *dev, const CorsairDeviceInfo *info, char *err, size_t err_len)
{
    PHIDP_PREPARSED_DATA preparsed = NULL;
    HIDP_CAPS caps;

    if (!dev || !info) {
        set_error(err, err_len, "Invalid device.");
        return false;
    }

    ZeroMemory(dev, sizeof(*dev));
    dev->handle = INVALID_HANDLE_VALUE;

    dev->handle = CreateFileW(info->path, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                              NULL);
    if (dev->handle == INVALID_HANDLE_VALUE) {
        set_error(err, err_len, "Could not open HID device: %lu. Close iCUE or other controller software.",
                  GetLastError());
        return false;
    }

    if (!HidD_GetPreparsedData(dev->handle, &preparsed)) {
        set_error(err, err_len, "HidD_GetPreparsedData failed: %lu", GetLastError());
        corsair_close(dev);
        return false;
    }

    if (HidP_GetCaps(preparsed, &caps) != HIDP_STATUS_SUCCESS) {
        HidD_FreePreparsedData(preparsed);
        set_error(err, err_len, "HidP_GetCaps failed.");
        corsair_close(dev);
        return false;
    }
    HidD_FreePreparsedData(preparsed);

    dev->input_report_len = caps.InputReportByteLength ? caps.InputReportByteLength : 17;
    dev->output_report_len = caps.OutputReportByteLength ? caps.OutputReportByteLength : 65;
    dev->read_ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    dev->write_ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    dev->io_mutex = CreateMutexW(NULL, FALSE, L"Global\\CorsairLinkReadWriteGuardMutex");
    if (!dev->io_mutex) {
        dev->io_mutex = CreateMutexW(NULL, FALSE, L"CorsairLinkReadWriteGuardMutex");
    }

    if (!dev->read_ov.hEvent || !dev->write_ov.hEvent || !dev->io_mutex) {
        set_error(err, err_len, "Could not create synchronization objects.");
        corsair_close(dev);
        return false;
    }

    HidD_SetNumInputBuffers(dev->handle, 16);
    dev->info = *info;
    return true;
}

void corsair_close(CorsairDevice *dev)
{
    if (!dev) {
        return;
    }

    if (dev->handle && dev->handle != INVALID_HANDLE_VALUE) {
        CancelIo(dev->handle);
        CloseHandle(dev->handle);
    }
    if (dev->read_ov.hEvent) {
        CloseHandle(dev->read_ov.hEvent);
    }
    if (dev->write_ov.hEvent) {
        CloseHandle(dev->write_ov.hEvent);
    }
    if (dev->io_mutex) {
        CloseHandle(dev->io_mutex);
    }
    ZeroMemory(dev, sizeof(*dev));
    dev->handle = INVALID_HANDLE_VALUE;
}

bool corsair_initialize(CorsairDevice *dev, char *err, size_t err_len)
{
    unsigned char res[RESPONSE_LEN];

    if (!send_command(dev, CMD_GET_FIRMWARE, NULL, 0, res, MUTEX_TIMEOUT_MS,
                      err, err_len)) {
        return false;
    }
    dev->status.firmware[0] = res[1];
    dev->status.firmware[1] = res[2];
    dev->status.firmware[2] = res[3];

    if (!send_command(dev, CMD_GET_BOOTLOADER, NULL, 0, res, MUTEX_TIMEOUT_MS,
                      err, err_len)) {
        return false;
    }
    dev->status.bootloader[0] = res[1];
    dev->status.bootloader[1] = res[2];

    if (!send_command(dev, CMD_GET_TEMP_CONFIG, NULL, 0, res, MUTEX_TIMEOUT_MS,
                      err, err_len)) {
        return false;
    }
    for (int i = 0; i < CORSAIR_TEMP_COUNT; ++i) {
        dev->status.temp_connected[i] = res[i + 1] != 0;
    }

    if (!send_command(dev, CMD_GET_FAN_MODES, NULL, 0, res, MUTEX_TIMEOUT_MS,
                      err, err_len)) {
        return false;
    }
    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        dev->status.fan_mode[i] = res[i + 1];
    }

    return corsair_refresh(dev, err, err_len);
}

bool corsair_refresh(CorsairDevice *dev, char *err, size_t err_len)
{
    unsigned char res[RESPONSE_LEN];

    for (int i = 0; i < CORSAIR_TEMP_COUNT; ++i) {
        dev->status.temp_c[i] = 0.0;
        if (dev->status.temp_connected[i]) {
            unsigned char data[1] = { (unsigned char)i };
            if (!send_command(dev, CMD_GET_TEMP, data, sizeof(data), res,
                              MUTEX_READ_TIMEOUT_MS, err, err_len)) {
                return false;
            }
            dev->status.temp_c[i] = (double)(int16_t)u16_be(res, 1) / 100.0;
        }
    }

    for (int i = 0; i < CORSAIR_FAN_COUNT; ++i) {
        dev->status.fan_rpm[i] = 0;
        if (dev->status.fan_mode[i] == CORSAIR_FAN_DC ||
            dev->status.fan_mode[i] == CORSAIR_FAN_PWM) {
            unsigned char data[1] = { (unsigned char)i };
            if (!send_command(dev, CMD_GET_FAN_RPM, data, sizeof(data), res,
                              MUTEX_READ_TIMEOUT_MS, err, err_len)) {
                return false;
            }
            dev->status.fan_rpm[i] = u16_be(res, 1);
        }
    }

    for (int i = 0; i < CORSAIR_VOLT_COUNT; ++i) {
        unsigned char data[1] = { (unsigned char)i };
        if (!send_command(dev, CMD_GET_VOLTS, data, sizeof(data), res,
                          MUTEX_READ_TIMEOUT_MS, err, err_len)) {
            return false;
        }
        dev->status.volts[i] = (double)u16_be(res, 1) / 1000.0;
    }

    return true;
}

bool corsair_set_fan_duty(CorsairDevice *dev, int fan_index, int duty_percent, char *err, size_t err_len)
{
    unsigned char data[2];
    unsigned char res[RESPONSE_LEN];

    if (fan_index < 0 || fan_index >= CORSAIR_FAN_COUNT) {
        set_error(err, err_len, "Invalid fan channel.");
        return false;
    }
    if (duty_percent < 0) {
        duty_percent = 0;
    }
    if (duty_percent > 100) {
        duty_percent = 100;
    }

    data[0] = (unsigned char)fan_index;
    data[1] = (unsigned char)duty_percent;
    return send_command(dev, CMD_SET_FAN_DUTY, data, sizeof(data), res,
                        MUTEX_TIMEOUT_MS, err, err_len);
}

bool corsair_set_fan_mode(CorsairDevice *dev, int fan_index, CorsairFanMode mode, char *err, size_t err_len)
{
    unsigned char data[3];
    unsigned char res[RESPONSE_LEN];

    if (fan_index < 0 || fan_index >= CORSAIR_FAN_COUNT) {
        set_error(err, err_len, "Invalid fan channel.");
        return false;
    }

    data[0] = 0x02;
    data[1] = (unsigned char)fan_index;
    data[2] = (unsigned char)mode;

    if (!send_command(dev, CMD_SET_FAN_MODE, data, sizeof(data), res,
                      MUTEX_TIMEOUT_MS, err, err_len)) {
        return false;
    }

    dev->status.fan_mode[fan_index] = mode;
    return true;
}
