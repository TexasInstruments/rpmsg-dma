#ifndef TI_RPMSG_RPC_PROTOCOL_H_
#define TI_RPMSG_RPC_PROTOCOL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TI_RPMSG_RPC_MAX_BUFFERS
#define TI_RPMSG_RPC_MAX_BUFFERS               (8U)
#endif

#define TI_RPMSG_RPC_PROTOCOL_MAGIC            (0x4F464C44U)
#define TI_RPMSG_RPC_PROTOCOL_VERSION          (1U)
#define TI_RPMSG_RPC_SERVICE_GENERIC           (1U)
#define TI_RPMSG_RPC_SERVICE_TVM               (2U)

#define TI_RPMSG_RPC_GENERIC_OP_EXECUTE             (1U)
#define TI_RPMSG_RPC_GENERIC_OP_EXECUTE_WITH_PARAMS (2U)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t service;
    uint16_t opcode;
    uint16_t flags;
    uint32_t sequence;
    int32_t status;
    uint32_t payload_size;
} TiRpmsg_Rpc_WireHeader;

typedef struct {
    uint64_t address;
    uint32_t size;
    uint32_t flags;
} TiRpmsg_Rpc_WireBufferDesc;

typedef struct {
    TiRpmsg_Rpc_WireHeader header;
    uint32_t kernel_id;
    uint16_t num_inputs;
    uint16_t num_outputs;
    uint32_t reserved;
    uint32_t reserved2;
    TiRpmsg_Rpc_WireBufferDesc inputs[TI_RPMSG_RPC_MAX_BUFFERS];
    TiRpmsg_Rpc_WireBufferDesc outputs[TI_RPMSG_RPC_MAX_BUFFERS];
} TiRpmsg_Rpc_GenericExecuteMessage;

typedef struct {
    TiRpmsg_Rpc_WireHeader header;
    uint32_t kernel_id;
    uint16_t num_inputs;
    uint16_t num_outputs;
    uint32_t params_size;
    uint32_t reserved;
    TiRpmsg_Rpc_WireBufferDesc inputs[TI_RPMSG_RPC_MAX_BUFFERS];
    TiRpmsg_Rpc_WireBufferDesc outputs[TI_RPMSG_RPC_MAX_BUFFERS];
} TiRpmsg_Rpc_GenericExecuteParamsMessage;

#if defined(__cplusplus)
static_assert(sizeof(TiRpmsg_Rpc_WireHeader) == 24U, "TI-Rpmsg-Rpc wire-header ABI changed");
static_assert(sizeof(TiRpmsg_Rpc_WireBufferDesc) == 16U, "TI-Rpmsg-Rpc buffer descriptor ABI changed");
static_assert(sizeof(TiRpmsg_Rpc_GenericExecuteMessage) == 296U, "TI-Rpmsg-Rpc execute ABI changed");
static_assert(sizeof(TiRpmsg_Rpc_GenericExecuteParamsMessage) == 296U, "TI-Rpmsg-Rpc execute-with-params ABI changed");
#else
_Static_assert(sizeof(TiRpmsg_Rpc_WireHeader) == 24U, "TI-Rpmsg-Rpc wire-header ABI changed");
_Static_assert(sizeof(TiRpmsg_Rpc_WireBufferDesc) == 16U, "TI-Rpmsg-Rpc buffer descriptor ABI changed");
_Static_assert(sizeof(TiRpmsg_Rpc_GenericExecuteMessage) == 296U, "TI-Rpmsg-Rpc execute ABI changed");
_Static_assert(sizeof(TiRpmsg_Rpc_GenericExecuteParamsMessage) == 296U, "TI-Rpmsg-Rpc execute-with-params ABI changed");
#endif

#ifdef __cplusplus
}
#endif

#endif /* TI_RPMSG_RPC_PROTOCOL_H_ */
