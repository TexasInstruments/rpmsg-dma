#ifndef RPMSG_2DFFT_EXAMPLE_H
#define RPMSG_2DFFT_EXAMPLE_H

#include <stdint.h>

#define DEBUG 0
#if DEBUG
#define DBG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define DBG(fmt, ...)
#endif

#define TI_RPMSG_RPC_PROTOCOL_MAGIC       (0x4F464C44U)
#define TI_RPMSG_RPC_PROTOCOL_VERSION     (1U)
#define TI_RPMSG_RPC_SERVICE_GENERIC      (1U)
#define TI_RPMSG_RPC_GENERIC_OP_EXECUTE   (1U)
#define TI_RPMSG_RPC_MAX_BUFFERS          (8U)
#define FFT2D_KERNEL_ID                 (1U)

/* Local (host) descriptor of the buffer */
typedef struct {
	uint32_t *data_buf;	/* mmaped tx dma-buf */
	uint32_t *params_buf;	/* mmaped rx dma-buf */
	int data_size;		/* Data dma-buf size */
	int params_size;	/* Param dma-buf size */
} local_buf_t;

//------- Define EQ control params structure --------
typedef struct __attribute__((__packed__))
{
	int32_t dsp_load;
	int32_t cycle_count;
	float ddr_throughput;
}
params_t;

/*
 * Wire protocol shared with source/edge_ai/offload/include/
 * ti_rpmsg_rpc_protocol.h in MCU+ SDK.
 *
 * Keep the explicit reserved2 field. It removes ABI-dependent padding before
 * the uint64_t buffer descriptors.
 */
typedef struct
{
	uint32_t magic;
	uint16_t version;
	uint16_t service;
	uint16_t opcode;
	uint16_t flags;
	uint32_t sequence;
	int32_t status;
	uint32_t payload_size;
} ti_rpmsg_rpc_wire_header_t;

typedef struct
{
	uint64_t address;
	uint32_t size;
	uint32_t flags;
} ti_rpmsg_rpc_wire_buffer_desc_t;

typedef struct
{
	ti_rpmsg_rpc_wire_header_t header;
	uint32_t kernel_id;
	uint16_t num_inputs;
	uint16_t num_outputs;
	uint32_t reserved;
	uint32_t reserved2;
	ti_rpmsg_rpc_wire_buffer_desc_t inputs[TI_RPMSG_RPC_MAX_BUFFERS];
	ti_rpmsg_rpc_wire_buffer_desc_t outputs[TI_RPMSG_RPC_MAX_BUFFERS];
} ti_rpmsg_rpc_generic_execute_msg_t;

static int rpmsg_fd = -1;
params_t *dspParams;
local_buf_t lbuf;
ti_rpmsg_rpc_generic_execute_msg_t offload_msg;


#endif //RPMSG_2DFFT_EXAMPLE_H
