#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

#define CORSAIR_MAX_DEVICES 16
#define CORSAIR_FAN_COUNT 6
#define CORSAIR_TEMP_COUNT 4
#define CORSAIR_VOLT_COUNT 3

typedef enum CorsairFanMode {
    CORSAIR_FAN_DISCONNECTED = 0,
    CORSAIR_FAN_DC = 1,
    CORSAIR_FAN_PWM = 2
} CorsairFanMode;

typedef struct CorsairDeviceInfo {
    wchar_t path[512];
    wchar_t product[128];
    wchar_t model[64];
    uint16_t vendor_id;
    uint16_t product_id;
} CorsairDeviceInfo;

typedef struct CorsairStatus {
    int firmware[3];
    int bootloader[2];
    int temp_connected[CORSAIR_TEMP_COUNT];
    double temp_c[CORSAIR_TEMP_COUNT];
    int fan_mode[CORSAIR_FAN_COUNT];
    int fan_rpm[CORSAIR_FAN_COUNT];
    double volts[CORSAIR_VOLT_COUNT];
} CorsairStatus;

typedef struct CorsairDevice {
    HANDLE handle;
    HANDLE io_mutex;
    OVERLAPPED read_ov;
    OVERLAPPED write_ov;
    unsigned short input_report_len;
    unsigned short output_report_len;
    CorsairDeviceInfo info;
    CorsairStatus status;
} CorsairDevice;

int corsair_find_devices(CorsairDeviceInfo *devices, int max_devices, char *err, size_t err_len);
bool corsair_open(CorsairDevice *dev, const CorsairDeviceInfo *info, char *err, size_t err_len);
void corsair_close(CorsairDevice *dev);
bool corsair_initialize(CorsairDevice *dev, char *err, size_t err_len);
bool corsair_refresh(CorsairDevice *dev, char *err, size_t err_len);
bool corsair_set_fan_duty(CorsairDevice *dev, int fan_index, int duty_percent, char *err, size_t err_len);
bool corsair_set_fan_mode(CorsairDevice *dev, int fan_index, CorsairFanMode mode, char *err, size_t err_len);
const wchar_t *corsair_fan_mode_name(int mode);
