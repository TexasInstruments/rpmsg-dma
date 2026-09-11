/*
 * @file rpmsg_2dfft_example.c
 * @brief Example implementation of a 2D FFT using RPMsg communication.
 *
 * This file demonstrates the usage of RPMsg for performing a 2D FFT operation.
 * It includes initialization, DMA buffer management, and cleanup routines.
 * The implementation is fully aligned with the audio_offload reference example.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <math.h>

#include "rpmsg_2dfft_example.h"
#include "rpmsg.h"
#include "dmabuf.h"
#include "fw_loader.h"

/** @brief Path to the input file containing FFT data. */
#define INPUT_FILE		"/usr/share/2dfft_test_data/2dfft_input_data.bin"

/** @brief Path to the file containing expected output data for validation. */
#define EXPECTED_OUTPUT_FILE	"/usr/share/2dfft_test_data/2dfft_expected_output_data.bin"

/** @brief Path to the base firmware for the C7x processor. */
#define C7_BASE_FW		"/lib/firmware/ti-ipc/am62dxx/dsp_edgeai_c7x_1_release_strip.out"

/** @brief Path to the test firmware for the 2D FFT operation. */
#define C7_TEST_FW		"/lib/firmware/fft2d_linux_dsp_offload_example.c75ss0-0.release.strip.out"

/** @brief Symbolic link to the firmware for the C7x processor. */
#define C7_FW_LINK		"/lib/firmware/am62d-c71_0-fw"

/** @brief Path to the state file for the remote processor. */
#define C7_FW_STATE		"/sys/class/remoteproc/remoteproc0/state"

/** @brief Device file for the remote processor. */
#define RPROC_NAME		"/dev/remoteproc0"

/** @brief Name of the DMA heap used for buffer allocation. */
#define DMA_HEAP_NAME		"linux,cma"

/** @brief Width of the 2D FFT input data. */
#define WIDTH			128

/** @brief Height of the 2D FFT input data. */
#define HEIGHT			128

/** @brief Total number of elements in the 2D FFT input data. */
#define ELEMENTS		(WIDTH * HEIGHT * 2)

/** @brief Size of the data buffer in bytes. */
#define DATA_BUF_SIZE		(ELEMENTS * sizeof(float))

/** @brief Size of the parameter buffer in bytes. */
#define PARAM_BUF_SIZE		(128)

/** @brief Processor ID for the C7x processor. */
#define C7_PROC_ID		8

/** @brief Remote endpoint ID for RPMsg communication. */
#define RMT_EP			14

/** @brief Structure for managing the data buffer. */
struct dma_buf_params  data_buf;

/** @brief Structure for managing the parameter buffer. */
struct dma_buf_params  param_buf;

/**
 * @brief Cleans up resources and switches firmware back to the base version.
 *
 * This function is called during program termination to release resources,
 * destroy DMA buffers, and reset the firmware to its base state.
 */
static void cleanup()
{
	switch_firmware(C7_BASE_FW, C7_FW_LINK, C7_FW_STATE);
	dmabuf_heap_destroy(&param_buf);
	dmabuf_heap_destroy(&data_buf);
	close(rpmsg_fd);
}

/**
 * @brief Signal handler for SIGINT (Ctrl+C).
 *
 * This function handles the SIGINT signal, performs cleanup, and exits the program.
 *
 * @param sig Signal number.
 */
void handle_sigint(int sig) {
	DBG("\n Caught signal %d (Ctrl+C). Cleaning up...\n", sig);
        cleanup();
        exit(0);
}

/**
 * @brief Loads test data into the DMA buffer.
 *
 * This function reads the input binary file and loads the data into the DMA buffer.
 *
 * @param file_path Path to the input binary file.
 * @param buffer Pointer to the DMA buffer.
 * @param elements Number of elements to load.
 * @return 0 on success, negative value on failure.
 */
static bool load_test_data(const char *filename, void *buffer, int elements) {
	FILE *f = fopen(filename, "rb");
	if (!f) {
		perror("Failed to open binary file");
		return false;
	}
	size_t read = fread(buffer, sizeof(float), elements, f);
	fclose(f);
	return (read == elements);
}

/**
 * @brief Compares the output data with the expected output file.
 *
 * This function validates the output data by comparing it with the expected output file.
 *
 * @param output Pointer to the output data buffer.
 * @param expected_file Path to the expected output file.
 * @param elements Number of elements to compare.
 * @return true if the output matches the expected data, false otherwise.
 */
static bool compare_output(const float *out_buf, const char *expected_file, int elements)
{
	float *expected = malloc(elements * sizeof(float));
	if (!expected) {
		perror("Failed to allocate memory for expected output");
		return false;
	}
	if (!load_test_data(expected_file, expected, elements)) {
		free(expected);
		return false;
	}
	for(int i = 0; i < elements; i ++)
		if(fabs(out_buf[i] - expected[i]) > 0.01 ) {
			free(expected);
			return false;
		}
	free(expected);
	return true;
}

/**
 * @brief Initializes the RPMsg buffer for communication.
 *
 * This function sets up the RPMsg buffer required for communication between
 * the host and the remote processor.
 */
void init_rpmsg_buffer()
{
        lbuf.data_buf = data_buf.kern_addr;
        lbuf.params_buf = param_buf.kern_addr ;
        lbuf.data_size = data_buf.size;
        lbuf.params_size = param_buf.size;

        ibuf.data_buffer = (uint32_t)data_buf.phys_addr;
        ibuf.params_buffer = (uint32_t)param_buf.phys_addr;
        ibuf.data_size = data_buf.size;
        ibuf.params_size = param_buf.size;

        dspParams = (params_t*)lbuf.params_buf;
}

/**
 * @brief Entry point for the RPMsg-based 2D FFT offload example.
 *
 * This function initializes the environment, loads the test firmware, sets up
 * DMA buffers, and performs the 2D FFT operation using RPMsg communication.
 * It validates the output against the expected results and cleans up resources
 * before exiting.
 *
 * @return 0 on success, non-zero on failure.
 */
int main()
{
	int ret = -1;

	printf("RPMsg based 2D FFT Offload Example\n");

	// Load the test firmware for the C7x processor
	if(switch_firmware(C7_TEST_FW, C7_FW_LINK, C7_FW_STATE) < 0) {
		fprintf(stderr, "Failed to load C7 firmware\n");
		goto switch_fw_fail;
	}
	sleep(1);

	 // Initialize the RPMsg
	rpmsg_fd = init_rpmsg(C7_PROC_ID, RMT_EP);
	if(rpmsg_fd < 0) {
		fprintf(stderr, "Failed to initialize rpmsg\n");
		return rpmsg_fd;
	}

	 // Initialize DMA buffers for data and parameters
	if(dmabuf_heap_init(DMA_HEAP_NAME, DATA_BUF_SIZE, RPROC_NAME, &data_buf) < 0)
	{
		fprintf(stderr, "Failed to initialize data DMA buffer\n");
		goto data_dma_allocate_fail;
	}
	if(dmabuf_heap_init(DMA_HEAP_NAME, PARAM_BUF_SIZE, RPROC_NAME, &param_buf) < 0)
	{
		fprintf(stderr, "Failed to initialize params DMA buffer\n");
		goto param_dma_allocate_fail;
	}

	// Initialize the RPMsg buffer for communication
        init_rpmsg_buffer();

        DBG("dmabuf for data buffer::  Kernel: %p Phy: 0x%x Size = %d\n",
			lbuf.data_buf, ibuf.data_buffer, lbuf.data_size);
        DBG("dmabuf for params buffer::  Kernel: %p Phy: 0x%x Size = %d\n",
			lbuf.params_buf, ibuf.params_buffer, lbuf.params_size);

	// Load input data into the DMA buffer
	dmabuf_sync(data_buf.dma_buf_fd, DMA_BUF_SYNC_START);
	ret = load_test_data(INPUT_FILE, data_buf.kern_addr, ELEMENTS);
	dmabuf_sync(data_buf.dma_buf_fd, DMA_BUF_SYNC_END);
	if(ret < 0) {
		fprintf(stderr, "Failed to load input binary\n");
		goto test_data_load_fail;
	}

	// Register the signal handler for SIGINT (Ctrl+C)
        signal(SIGINT, handle_sigint);

	int packet_len;
	packet_len = sizeof(ibuf);

	// Perform the 2D FFT operation using RPMsg communication
	ret = send_msg(rpmsg_fd, (char *)&ibuf, sizeof(ibuf));
	if (ret < 0) {
		printf("send_msg failed, ret = %d\n", ret);
		goto rpmsg_snd_recv_fail;
	}
	if (ret != packet_len) {
		printf("bytes written does not match send request, ret = %d, packet_len = %d\n", ret, packet_len);
		goto rpmsg_snd_recv_fail;
	}
	ret = recv_msg(rpmsg_fd, 256, (char *)&ibuf, &packet_len);
	if (ret < 0) {
		printf("recv_msg failed ret = %d\n", ret);
		goto rpmsg_snd_recv_fail;
	}

	dmabuf_sync(data_buf.dma_buf_fd, DMA_BUF_SYNC_START);
	dmabuf_sync(param_buf.dma_buf_fd, DMA_BUF_SYNC_START);

	// Compare the output data with the expected results
	bool pass = compare_output((float *)data_buf.kern_addr, EXPECTED_OUTPUT_FILE, ELEMENTS);

	// Print the test result
	printf("\n*****************************************\n");
	printf("*****************************************\n");
	printf("\nC7x 2DFFT Test %s\n", pass ? "PASSED" : "FAILED");
	printf("C7x Load: %d%% \n", (dspParams->dsp_load / 100));
	printf("C7x Cycle Count: %d\n", dspParams->cycle_count);
	printf("C7x DDR Throughput: %f MB/s\n", dspParams->ddr_throughput);
	printf("\n*****************************************\n");
	printf("*****************************************\n\n");

	dmabuf_sync(data_buf.dma_buf_fd, DMA_BUF_SYNC_END);
	dmabuf_sync(param_buf.dma_buf_fd, DMA_BUF_SYNC_END);

	// Clean up resources before exiting
	cleanup();

	return pass ? 0 : 1;

rpmsg_snd_recv_fail:
test_data_load_fail:
	switch_firmware(C7_BASE_FW, C7_FW_LINK, C7_FW_STATE);
switch_fw_fail:
	dmabuf_heap_destroy(&param_buf);
param_dma_allocate_fail:
	dmabuf_heap_destroy(&data_buf);
data_dma_allocate_fail:
	close(rpmsg_fd);
	return ret;
}
