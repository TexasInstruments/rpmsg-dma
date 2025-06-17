#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config.h"

AppConfig app_config = {0};

// ========== Helper ==========
static void trim(char *str)
{
	char *end;
	while (isspace((unsigned char)*str)) str++;
	if (*str == 0) return;
	end = str + strlen(str) - 1;
	while (end > str && isspace((unsigned char)*end)) end--;
	*(end + 1) = '\0';
}

void init_config_defaults()
{
	app_config.pcm_device = strdup("default");
	app_config.uart_device = strdup("/dev/ttyS2");
	app_config.rproc_dev_name = strdup("/dev/remoteproc0");
	app_config.dma_heap_reserved = strdup("linux,cma");
	app_config.sample_audio_file = strdup("/opt/sample.wav");
	app_config.fw_link_path = strdup("/lib/firmware/am62a-c71_0-fw");
	app_config.c7_old_fw_path = strdup("/lib/firmware/ti-ipc/am62axx-c71-fw-old.xe71");
	app_config.c7_new_fw_path = strdup("/lib/firmware/ti-ipc/am62axx-c71-fw-new.xe71");
	app_config.c7_state_path= strdup("/sys/class/remoteproc/remoteproc0/state");
	app_config.c7_proc_id = 8;
	app_config.remote_endpoint = 14;
	app_config.data_buffer_size = 4096;
	app_config.param_buffer_size = 4096;
	app_config.fft_bin_index = 0;
	app_config.is_host_eth_iface = true;
	app_config.is_dsp_execution = true;
}

// ========== Config Loader ==========
void load_config(const char *filename)
{
	FILE *fp = fopen(filename, "r");
	if (!fp) {
		perror("Failed to open config file");
		return;
	}

	char line[256], key[64], val[128];
	while (fgets(line, sizeof(line), fp)) {
		trim(line);
		if (line[0] == '#' || line[0] == '\0') continue;
		if (sscanf(line, "%63[^=]=%127[^\n]", key, val) == 2) {
			trim(key);
			trim(val);
			// Strings
			if (strcmp(key, "PCM_DEVICE") == 0) {
				free(app_config.pcm_device);
				app_config.pcm_device = strdup(val);
			}
			else if (strcmp(key, "UART_DEVICE") == 0) {
				free(app_config.uart_device);
				app_config.uart_device = strdup(val);
			}
			else if (strcmp(key, "RPROC_DEV_NAME") == 0) {
				free(app_config.rproc_dev_name);
				app_config.rproc_dev_name = strdup(val);
			}
			else if (strcmp(key, "DMA_HEAP_RESERVED") == 0) {
				free(app_config.dma_heap_reserved);
				app_config.dma_heap_reserved = strdup(val);
			}
			else if (strcmp(key, "SAMPLE_AUDIO_FILE") == 0) {
				free(app_config.sample_audio_file);
				app_config.sample_audio_file = strdup(val);
			}
			else if (strcmp(key, "FW_LINK_PATH") == 0) {
				free(app_config.fw_link_path);
				app_config.fw_link_path = strdup(val);
			}
			else if (strcmp(key, "C7_OLD_FW_PATH") == 0) {
				free(app_config.c7_old_fw_path);
				app_config.c7_old_fw_path = strdup(val);
			}
			else if (strcmp(key, "C7_NEW_FW_PATH") == 0) {
				free(app_config.c7_new_fw_path);
				app_config.c7_new_fw_path = strdup(val);
			}
			else if (strcmp(key, "C7_STATE_PATH") == 0) {
				free(app_config.c7_state_path);
				app_config.c7_state_path = strdup(val);
			}
			// Integers
			else if (strcmp(key, "C7_PROC_ID") == 0) app_config.c7_proc_id = atoi(val);
			else if (strcmp(key, "REMOTE_ENDPT") == 0) app_config.remote_endpoint = atoi(val);
			else if (strcmp(key, "DATA_SIZE") == 0) app_config.data_buffer_size = atoi(val);
			else if (strcmp(key, "PARAM_SIZE") == 0) app_config.param_buffer_size = atoi(val);
			else if (strcmp(key, "DSP_EXEC_MODE") == 0) app_config.is_dsp_execution = atoi(val);
			else if (strcmp(key, "HOST_ETH_INTERFACE") == 0) app_config.is_host_eth_iface = atoi(val);
			else if (strcmp(key, "sample_audio_file") == 0) app_config.fft_bin_index = atoi(val);
		}
	}
	fclose(fp);
}

void print_config()
{
	printf("PCM Device       : %s\n", app_config.pcm_device);
	printf("UART Device       : %s\n", app_config.uart_device);
	printf("Remoteproc Device : %s\n", app_config.rproc_dev_name);
	printf("DMA Heap Reserved : %s\n", app_config.dma_heap_reserved);
	printf("Sample Audio File : %s\n", app_config.sample_audio_file);
	printf("Remote endpoint : %d\n",app_config.remote_endpoint);
	printf("Date buffer size : %d\n", app_config.data_buffer_size);
	printf("param buffer size : %d\n", app_config.param_buffer_size);
	printf("is DSP mode execution : %d\n", app_config.is_dsp_execution);
	printf("Host eth interface : %d\n", app_config.is_host_eth_iface);
	printf("FFT length : %d\n", app_config.fft_bin_index);
	printf("C7 new : %s\n", app_config.c7_new_fw_path);
	printf("C7 old : %s\n", app_config.c7_old_fw_path);
	printf("C7 state : %s\n", app_config.c7_state_path);
	printf("C7 link : %s\n", app_config.fw_link_path);
}

void cleanup_config()
{
	free(app_config.pcm_device);
	free(app_config.uart_device);
	free(app_config.rproc_dev_name);
	free(app_config.dma_heap_reserved);
	free(app_config.sample_audio_file);
}
