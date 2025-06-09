#ifndef CONFIG_H
#define CONFIG_H

#include<stdbool.h>

typedef struct {
	char *pcm_device;
	char *uart_device;
	char *rproc_dev_name;
	char *dma_heap_reserved;
	char *sample_audio_file;
	char *fw_link_path;
	char *c7_old_fw_path;
	char *c7_new_fw_path;
	char *c7_state_path;

	int c7_proc_id;
	int remote_endpoint;
	int data_buffer_size;
	int param_buffer_size;
	int fft_bin_index;
	bool is_host_eth_iface;
	bool is_dsp_execution;
} AppConfig;

extern AppConfig app_config;
// ========== Function ==========
void load_config(const char *filename);
void print_config();
void cleanup_config();

#endif // CONFIG_H
