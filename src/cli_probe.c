#include "corsair_hid.h"
#include "nvidia_temp.h"

#include <stdio.h>
#include <windows.h>

static void print_wide_utf8(const wchar_t *text)
{
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, (int)sizeof(buf), NULL, NULL);
    fputs(buf, stdout);
}

int main(void)
{
    CorsairDeviceInfo devices[CORSAIR_MAX_DEVICES];
    int count;
    char err[256] = { 0 };

    SetConsoleOutputCP(CP_UTF8);

    count = corsair_find_devices(devices, CORSAIR_MAX_DEVICES, err, sizeof(err));
    if (count <= 0) {
        printf("No supported devices: %s\n", err);
        return 1;
    }

    printf("Found %d supported device(s).\n", count);

    for (int i = 0; i < count; ++i) {
        CorsairDevice dev;

        printf("\n[%d] ", i);
        print_wide_utf8(devices[i].model);
        printf(" VID_%04X PID_%04X\n", devices[i].vendor_id, devices[i].product_id);

        if (!corsair_open(&dev, &devices[i], err, sizeof(err))) {
            printf("  open failed: %s\n", err);
            continue;
        }

        if (!corsair_initialize(&dev, err, sizeof(err))) {
            printf("  initialize failed: %s\n", err);
            corsair_close(&dev);
            continue;
        }

        printf("  firmware: %d.%d.%d\n",
               dev.status.firmware[0], dev.status.firmware[1], dev.status.firmware[2]);
        printf("  bootloader: %d.%d\n",
               dev.status.bootloader[0], dev.status.bootloader[1]);

        for (int fan = 0; fan < CORSAIR_FAN_COUNT; ++fan) {
            printf("  fan%d: ", fan + 1);
            print_wide_utf8(corsair_fan_mode_name(dev.status.fan_mode[fan]));
            printf(", %d rpm\n", dev.status.fan_rpm[fan]);
        }

        for (int temp = 0; temp < CORSAIR_TEMP_COUNT; ++temp) {
            if (dev.status.temp_connected[temp]) {
                printf("  temp%d: %.1f C\n", temp + 1, dev.status.temp_c[temp]);
            } else {
                printf("  temp%d: N/A\n", temp + 1);
            }
        }

        printf("  rails: +12V %.2f V, +5V %.2f V, +3.3V %.2f V\n",
               dev.status.volts[0], dev.status.volts[1], dev.status.volts[2]);

        corsair_close(&dev);
    }

    NvidiaGpuStatus gpu;
    if (nvidia_temp_read(&gpu, err, sizeof(err))) {
        printf("\nNVIDIA: %s, %d C", gpu.name, gpu.temperature_c);
        if (gpu.gpu_count > 1) {
            printf(" (%d GPUs found, using first)", gpu.gpu_count);
        }
        printf("\n");
    } else {
        printf("\nNVIDIA: unavailable (%s)\n", err);
    }

    return 0;
}
