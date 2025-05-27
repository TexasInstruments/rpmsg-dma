#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

float get_cpu_load();
void log_superbuf(int16_t *superbuf);
void update_metrics(float lat, float amp, float cpu, float dsp,
                    double *total_latency, double *min_latency, double *max_latency,
                    double *total_amp, double *min_amp, double *max_amp,
                    double *total_cpu, double *min_cpu, double *max_cpu,
                    double *total_dsp, double *min_dsp, double *max_dsp);

void log_frame_metrics(int exec_mode, int frames, float amp, float lat, float cpu, float dsp);
void log_summary(int frames, double total_latency, double min_latency, double max_latency,
                 double total_amp, double min_amp, double max_amp,
                 double total_cpu, double min_cpu, double max_cpu,
                 double total_dsp, double min_dsp, double max_dsp);
