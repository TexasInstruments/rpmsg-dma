#include <stdio.h>
#include <stdint.h>
#include "metrics.h"
#include "host_interface.h"

#define FRAME_SIZE              256

void send_wave_in(int16_t *inbuf)
{
	char line[2048];
	int off = snprintf(line, sizeof(line), "IWAVE:");
	for (int i = 0; i < FRAME_SIZE; ++i) {
		off += snprintf(line + off, sizeof(line) - off,
		i == FRAME_SIZE - 1 ? "%d\n" : "%d,", inbuf[i]);
		if (off >= (int)sizeof(line)) break;
	}
	enqueue_log(line);
}

void send_wave_out(int16_t *outbuf)
{
	char line[2048];
	int off = snprintf(line, sizeof(line), "WAVE:");
	for (int i = 0; i < FRAME_SIZE; ++i) {
		off += snprintf(line + off, sizeof(line) - off,
		i == FRAME_SIZE - 1 ? "%d\n" : "%d,", outbuf[i]);
		if (off >= (int)sizeof(line)) break;
	}
	enqueue_log(line);
}

// ====================== Metrics & Logging  =========================

float get_cpu_load()
{
	static long last_user=0, last_nice=0, last_system=0, last_idle=0;
	long user, nice, system, idle;
	FILE *fp = fopen("/proc/stat", "r");
	if (!fp) return -1;
	fscanf(fp, "cpu %ld %ld %ld %ld", &user, &nice, &system, &idle);
	fclose(fp);
	long total = (user-last_user)+(nice-last_nice)+(system-last_system);
	long total_all = total + (idle-last_idle);
	last_user=user;
	last_nice=nice;
	last_system=system;
	last_idle=idle;
	return total_all ? (100.0f * total / total_all) : 0.0f;
}

void update_metrics(float lat, float amp, float cpu, float dsp,
                    double *total_latency, double *min_latency, double *max_latency,
                    double *total_amp, double *min_amp, double *max_amp,
                    double *total_cpu, double *min_cpu, double *max_cpu,
                    double *total_dsp, double *min_dsp, double *max_dsp)
{
	*total_latency += lat;
	if (lat < *min_latency) *min_latency = lat;
	if (lat > *max_latency) *max_latency = lat;

	*total_amp += amp;
	if (amp < *min_amp) *min_amp = amp;
	if (amp > *max_amp) *max_amp = amp;

	*total_cpu += cpu;
	if (cpu < *min_cpu) *min_cpu = cpu;
	if (cpu > *max_cpu) *max_cpu = cpu;

	*total_dsp += dsp;
	if (dsp < *min_dsp) *min_dsp = dsp;
	if (dsp > *max_dsp) *max_dsp = dsp;
}

void log_superbuf(int16_t *superbuf)
{
	send_wave_in(&superbuf[0]);
	send_wave_out(&superbuf[FRAME_SIZE]);
}

void log_frame_metrics(int exec_mode, int frames, float amp, float lat, float cpu, float dsp)
{
	char logbuf[256];
	snprintf(logbuf, sizeof(logbuf),
	         "Frame %d: AvgAmp=%.2f, Latency=%.2fms, Mode=%s CPULoad=%.1f%% DSPLoad=%.1f%%",
	         frames, amp, lat, exec_mode == 0 ? "CPU" : "DSP", cpu, dsp);
	enqueue_log(logbuf);
}

void log_summary(int frames, double total_latency, double min_latency, double max_latency,
                 double total_amp, double min_amp, double max_amp,
                 double total_cpu, double min_cpu, double max_cpu,
                 double total_dsp, double min_dsp, double max_dsp)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "[Live Summary] Frames: %d", frames);
	enqueue_log(buf);
	snprintf(buf, sizeof(buf), "[Live Summary] Latency (ms): Min: %.2f, Max: %.2f, Avg: %.2f",
	         min_latency, max_latency, total_latency / frames);
	enqueue_log(buf);
	snprintf(buf, sizeof(buf), "[Live Summary] Amp: Min: %.2f, Max: %.2f, Avg: %.2f",
	         min_amp, max_amp, total_amp / frames);
	enqueue_log(buf);
	snprintf(buf, sizeof(buf), "[Live Summary] CPU Load (%%): Min: %.1f, Max: %.1f, Avg: %.1f",
	         min_cpu, max_cpu, total_cpu / frames);
	enqueue_log(buf);
	snprintf(buf, sizeof(buf), "[Live Summary] DSP Load (%%): Min: %.1f, Max: %.1f, Avg: %.1f",
	         min_dsp, max_dsp, total_dsp / frames);
	enqueue_log(buf);
}


