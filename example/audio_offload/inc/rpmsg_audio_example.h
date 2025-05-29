#ifndef RPMSG_AUDIO_EXAMPLE_H
#define RPMSG_AUDIO_EXAMPLE_H

char CFG_FILE_PATH[] = "/etc/dsp_offload.cfg";

#define FRAME_SIZE              256
#define SAMPLE_RATE             48000
#define SHM_SIZE                (FRAME_SIZE * sizeof(int16_t))
#define DSP_PARAMS_OFFSET       (2 * SHM_SIZE)

#define BASIC_EQ_FFT            1
#define BASIC_PITCH_SHIFTING    0

#define DEBUG 0
#if DEBUG
#define DBG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define DBG(fmt, ...)
#endif

enum {
	STOP_PLAY,
	START_PLAY,
	EXIT_PLAY,
};
/* Local (host) descriptor of the buffer */
typedef struct {
	uint32_t *data_buf;     /* mmaped tx dma-buf */
	uint32_t *params_buf;     /* mmaped rx dma-buf */
	int data_size;
	int params_size;                   /* Total dma-buf size */
} local_buf_t;

//------- Define EQ control params structure --------
typedef struct __attribute__((__packed__))
{
	float dsp_load;
	int32_t zeroFFTLength;
}
params_t;

//------- Define C7 IPC message structure --------
typedef struct __attribute__((__packed__))
{
	uint32_t data_buffer;
	uint32_t params_buffer;
	int32_t data_size;
	int32_t params_size;
	int32_t graph_id;

}
ipc_msg_buf_t;

typedef enum { EXEC_ARM, EXEC_DSP } ExecMode;
ExecMode current_mode = EXEC_ARM;

int32_t arm_zero_fft_index = 0;

#if BASIC_PITCH_SHIFTING
float pitch_shift_factor = 2.0f;
#endif

int start_requested = START_PLAY;

pthread_t eq_thread;
static int rpmsg_fd = -1;

void *map_base;
char *dev_name = NULL;

params_t *dspParams;
local_buf_t lbuf;
ipc_msg_buf_t ibuf;

#endif //RPMSG_AUDIO_EXAMPLE_H
