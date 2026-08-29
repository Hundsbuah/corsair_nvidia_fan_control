#pragma once

#include <stdbool.h>
#include <stddef.h>

/* NVML clocks-event-reason bits (nvmlDeviceGetCurrentClocksEventReasons).
 * Identifies which active constraint is limiting the GPU's clocks. */
#define NVML_EVENT_GPU_IDLE             0x0000000000000001ULL
#define NVML_EVENT_APPS_CLOCKS          0x0000000000000002ULL
#define NVML_EVENT_SW_POWER_CAP         0x0000000000000004ULL
#define NVML_EVENT_HW_SLOWDOWN          0x0000000000000008ULL
#define NVML_EVENT_SYNC_BOOST           0x0000000000000010ULL
#define NVML_EVENT_SW_THERMAL_SLOWDOWN  0x0000000000000020ULL
#define NVML_EVENT_HW_THERMAL_SLOWDOWN  0x0000000000000040ULL
#define NVML_EVENT_HW_POWER_BRAKE       0x0000000000000080ULL
#define NVML_EVENT_DISPLAY_CLOCK        0x0000000000000100ULL

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
    bool have_throttle; /* throttle status read succeeded                 */
    int gpu_clock_mhz;  /* current graphics clock                         */
    int mem_clock_mhz;  /* current memory clock                           */
    int fan_speed_pct;  /* 0-100                                          */
    int power_mw;       /* current board power draw                       */
    int power_limit_mw; /* configured power management limit              */
    unsigned long long throttle_status; /* active throttle reason bitmask */
    char err[128];
} GpuStats;

bool gpu_stats_read(GpuStats *stats);
void gpu_stats_shutdown(void);