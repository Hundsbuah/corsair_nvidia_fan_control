#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct NvidiaGpuStatus {
    bool available;
    int gpu_count;
    int temperature_c;
    char name[64];
} NvidiaGpuStatus;

bool nvidia_temp_read(NvidiaGpuStatus *status, char *err, size_t err_len);
