#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Live GPU telemetry from NVML (the API behind nvidia-smi). NVML is loaded
 * dynamically from nvml.dll, mirroring the dynamic NVAPI load in
 * nvidia_temp.c, so the binary stays free of driver-library link
 * dependencies. */
typedef struct GpuStats {
    bool available;     /* NVML session up and a device handle was found */
    bool have_clocks;   /* both clock reads succeeded                     */
    bool have_fan;      /* fan speed read succeeded                       */
    bool have_power;    /* current power draw read succeeded              */
    bool have_limit;    /* power management limit read succeeded          */
    int gpu_clock_mhz;  /* current graphics clock                         */
    int mem_clock_mhz;  /* current memory clock                           */
    int fan_speed_pct;  /* 0-100                                          */
    int power_mw;       /* current board power draw                       */
    int power_limit_mw; /* configured power management limit              */
    char err[128];
} GpuStats;

bool gpu_stats_read(GpuStats *stats);
void gpu_stats_shutdown(void);