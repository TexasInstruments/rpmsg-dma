#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include <sndfile.h>
#include <fftw3.h>
#include "config.h"
#include "rpmsg_audio_example.h"
#include "rpmsg.h"
#include "dmabuf.h"
#include "fw_loader.h"
#include "metrics.h"
#include "host_interface.h"
#include <signal.h>

#define SUPER_SIZE  (FRAME_SIZE * NUM_FRAMES * 2)
int16_t superbuf[SUPER_SIZE];
int current_channel = 0;
struct dma_buf_params  data_dma_buf_params;
struct dma_buf_params  options_dma_buf_params;
snd_pcm_t *pcm;
SNDFILE *sf;

void handle_sigint(int sig) {
	DBG("\n Caught signal %d (Ctrl+C). Cleaning up...\n", sig);
	if(current_mode) {
		switch_firmware(app_config.c7_old_fw_path,
                                app_config.fw_link_path, app_config.c7_state_path);
	}
	cleanup_config();
	exit(0);
}

void set_zero_fft_index(int32_t value)
{
	if(current_mode == EXEC_DSP) {
		dmabuf_sync(options_dma_buf_params.dma_buf_fd, DMA_BUF_SYNC_START);
		dspParams->zeroFFTLength = value;
		dmabuf_sync(options_dma_buf_params.dma_buf_fd, DMA_BUF_SYNC_END);
	} else
		arm_zero_fft_index = value;
}

// ====================== ARM-Side Audio Processing =======================

void process_on_arm()
{
	const int N = NUM_FRAMES;
	int16_t *data = (int16_t *)lbuf.data_buf;

	for (int ch = 0; ch < CHANNELS; ++ch) {
		double input[N], output[N];
		fftw_complex spectrum[N];
		fftw_plan fwd, bwd;

		for (int i = 0; i < N; ++i) {
			input[i] = (double)data[i * CHANNELS + ch];
		}

		fwd = fftw_plan_dft_r2c_1d(N, input, spectrum, FFTW_ESTIMATE);
		fftw_execute(fwd);

		for (int i = 0; i < arm_zero_fft_index; ++i) {
			spectrum[i][0] = 0;
			spectrum[i][1] = 0;
		}

		bwd = fftw_plan_dft_c2r_1d(N, spectrum, output, FFTW_ESTIMATE);
		fftw_execute(bwd);

		for (int i = 0; i < N; ++i) {
			int val = (int)(output[i] / N);
			if (val > 32767) val = 32767;
			if (val < -32768) val = -32768;
			data[i * CHANNELS + ch] = (int16_t)val;
		}
		fftw_destroy_plan(fwd);
		fftw_destroy_plan(bwd);
	}
}
void process_on_dsp()
{
	int ret = 0;
	int i = 0;
	int packet_len;
	packet_len = sizeof(ibuf);

	ret = send_msg(rpmsg_fd, (char *)&ibuf, sizeof(ibuf));
	if (ret < 0) {
		printf("send_msg failed for iteration %d, ret = %d\n", i, ret);
		return;
	}
	if (ret != packet_len) {
		printf("bytes written does not match send request, ret = %d, packet_len = %d\n", i, ret);
		return;
	}
	ret = recv_msg(rpmsg_fd, 256, (char *)&ibuf, &packet_len);

	if (ret < 0) {
		printf("recv_msg failed for iteration %d, ret = %d\n", i, ret);
		return;
	}
}


double time_diff_ms(struct timespec a, struct timespec b)
{
	return (b.tv_sec-a.tv_sec)*1000.0 + (b.tv_nsec-a.tv_nsec)/1e6;
}

void *run_eq_thread(void *arg)
{
	const char* input_file = (const char *)arg;
	double total_latency=0, min_latency=1e6, max_latency=0;
	int frames = 0;
	double total_amp = 0.0, min_amp = 1e9, max_amp = -1e9;
	double total_cpu = 0.0, min_cpu = 1e9, max_cpu = -1e9;
	double total_dsp = 0.0, min_dsp = 1e9, max_dsp = -1e9;
	sf_count_t frames_read;
	struct timespec t1, t2;

	SF_INFO sfinfo = {0};
	SNDFILE *infile = sf_open(input_file, SFM_READ, &sfinfo);
	if (!infile) {
		fprintf(stderr, "Failed to open input WAV: %s\n", sf_strerror(NULL));
		start_requested = EXIT_PLAY;
		pthread_exit(infile);
	}

	if (sfinfo.channels != CHANNELS || sfinfo.samplerate != SAMPLE_RATE) {
		fprintf(stderr, "WAV file must be %d-ch %dHz\n", CHANNELS, SAMPLE_RATE);
		sf_close(infile);
		start_requested = EXIT_PLAY;
		pthread_exit((void *)1);
	}

	snd_pcm_t *pcm_handle;
	int rc = snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
	if (rc < 0) {
		fprintf(stderr, "snd_pcm_open error: %s\n", snd_strerror(rc));
		sf_close(infile);
		start_requested = EXIT_PLAY;
		pthread_exit(&rc);
	}

	rc = snd_pcm_set_params(pcm_handle,
                            SND_PCM_FORMAT_S16_LE,
                            SND_PCM_ACCESS_RW_INTERLEAVED,
                            CHANNELS,
                            SAMPLE_RATE,
                            1,
                            500000);
	if (rc < 0) {
		fprintf(stderr, "snd_pcm_set_params error: %s\n", snd_strerror(rc));
		snd_pcm_close(pcm_handle);
		sf_close(infile);
		start_requested = EXIT_PLAY;
		pthread_exit(&rc);
	}

	while(1) {
		dmabuf_sync(data_dma_buf_params.dma_buf_fd, DMA_BUF_SYNC_START);
		frames_read = sf_readf_short(infile, (short *)lbuf.data_buf, NUM_FRAMES);
		if(frames_read != NUM_FRAMES)
			break;

		memcpy(&superbuf[0], lbuf.data_buf,  NUM_FRAMES * sizeof(int16_t));
		dmabuf_sync(data_dma_buf_params.dma_buf_fd, DMA_BUF_SYNC_END);

		clock_gettime(CLOCK_MONOTONIC, &t1);
		current_mode ? process_on_dsp() : process_on_arm();
		clock_gettime(CLOCK_MONOTONIC, &t2);

		double lat = time_diff_ms(t1, t2);
		long sum = 0;

		dmabuf_sync(data_dma_buf_params.dma_buf_fd, DMA_BUF_SYNC_START);
		dmabuf_sync(options_dma_buf_params.dma_buf_fd, DMA_BUF_SYNC_START);

		for (int i = 0; i < NUM_FRAMES; i++) sum += abs(lbuf.data_buf[i]);
		float amp = (float)sum / NUM_FRAMES;
		float cpu = get_cpu_load();
		float dsp = (current_mode == EXEC_DSP ?  dspParams->dsp_load : 0.0f);
		update_metrics(lat, amp, cpu, dsp, &total_latency, &min_latency, &max_latency,
		               &total_amp, &min_amp, &max_amp, &total_cpu, &min_cpu, &max_cpu, &total_dsp, &min_dsp, &max_dsp);

		log_frame_metrics(current_mode, ++frames, amp, lat, cpu, dsp);

		snd_pcm_writei(pcm_handle, (short *)lbuf.data_buf, frames_read);
		memcpy(&superbuf[256], lbuf.data_buf, NUM_FRAMES * sizeof(int16_t));
		log_superbuf(superbuf, NUM_FRAMES, CHANNELS, current_channel);
		dmabuf_sync(options_dma_buf_params.dma_buf_fd, DMA_BUF_SYNC_END);
		dmabuf_sync(data_dma_buf_params.dma_buf_fd, DMA_BUF_SYNC_END);
		if (frames % 10 == 0) {
			log_summary(frames, total_latency, min_latency, max_latency,
			            total_amp, min_amp, max_amp,
			            total_cpu, min_cpu, max_cpu,
			            total_dsp, min_dsp, max_dsp);
		}
	}
	snd_pcm_close(pcm_handle);
	sf_close(infile);
	printf("processing complete\n");
	start_requested = EXIT_PLAY;
	pthread_exit(0);
}

void init_rpmsg_buffer(int graph_id)
{
	lbuf.data_buf = data_dma_buf_params.kern_addr;
	lbuf.params_buf = options_dma_buf_params.kern_addr ;
	lbuf.data_size = data_dma_buf_params.size;
	lbuf.params_size = options_dma_buf_params.size;

	ibuf.data_buffer = (uint32_t)data_dma_buf_params.phys_addr;
	ibuf.params_buffer = (uint32_t)options_dma_buf_params.phys_addr;
	ibuf.data_size = data_dma_buf_params.size;
	ibuf.params_size = options_dma_buf_params.size;
	ibuf.graph_id = graph_id;

	dspParams = (params_t*)lbuf.params_buf;
}

// ============================== Main ====================================

int main(int argc, char **argv)
{
	const char* input_file = NULL;

	load_config(CFG_FILE_PATH);
	input_file = app_config.sample_audio_file;
	current_mode = (ExecMode)app_config.is_dsp_execution;

	// Register signal handler for SIGINT
	signal(SIGINT, handle_sigint);

	if(current_mode) {
		// Load Test firmware
		switch_firmware(app_config.c7_new_fw_path,
				app_config.fw_link_path, app_config.c7_state_path);
		sleep(1);
	}
	app_config.data_buffer_size = FRAME_SIZE * NUM_FRAMES;
	rpmsg_fd = init_rpmsg(app_config.c7_proc_id, app_config.remote_endpoint);
	dmabuf_heap_init(app_config.dma_heap_reserved,
			app_config.data_buffer_size, app_config.rproc_dev_name, &data_dma_buf_params);
	dmabuf_heap_init(app_config.dma_heap_reserved,
			app_config.param_buffer_size, app_config.rproc_dev_name, &options_dma_buf_params);
	init_rpmsg_buffer(0);
	init_host_interface();

	if(current_mode) {
		dmabuf_sync(options_dma_buf_params.dma_buf_fd, DMA_BUF_SYNC_END);
		dspParams->zeroFFTLength = app_config.fft_bin_index;
		dmabuf_sync(options_dma_buf_params.dma_buf_fd, DMA_BUF_SYNC_END);
	}

	DBG("dmabuf for data buffer::  Kernel: %p Phy: 0x%x Size = %d\n", lbuf.data_buf, ibuf.data_buffer, lbuf.data_size);
	DBG("dmabuf for params buffer::  Kernel: %p Phy: 0x%x Size = %d\n", lbuf.params_buf, ibuf.params_buffer, lbuf.params_size);

	DBG("Execution on : %s\n", current_mode ? "DSP" : "ARM");
	DBG("Audio File: %s\n", input_file);

	while (1)
	{
		if(start_requested == START_PLAY)
		{
			start_requested = STOP_PLAY;
			int ret = pthread_create(&eq_thread, NULL, run_eq_thread, (void *)input_file);
			if(ret != 0) {
				fprintf(stderr, "snd_pcm_open error: %s\n", snd_strerror(ret));
				break;
			}
		}
		else if(start_requested == EXIT_PLAY)
		{
			DBG("processing complete go to cleanup\n");
			break;
		}
		else
			sleep(2);
	}

	dmabuf_heap_destroy(&data_dma_buf_params);
	dmabuf_heap_destroy(&options_dma_buf_params);
	if(current_mode) {
		// Revert to original firmware
		switch_firmware(app_config.c7_old_fw_path,
                                app_config.fw_link_path, app_config.c7_state_path);
	}
	cleanup_config();
	return 0;
}

