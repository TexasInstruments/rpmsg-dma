#ifndef TI_RPMSG_RPC_CLIENT_H_
#define TI_RPMSG_RPC_CLIENT_H_

#include <stdint.h>
#include "ti_rpmsg_rpc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Execute one generic TI-Rpmsg-Rpc kernel over an already-open rpmsg-char fd.
 *
 * Returns 0 when a syntactically valid TI-Rpmsg-Rpc response is received.
 * The remote kernel/library status is returned through remote_status.
 * Local Linux/transport/protocol errors are returned as negative errno values.
 */
int ti_rpmsg_rpc_generic_execute(
    int rpmsg_fd,
    uint32_t sequence,
    uint32_t kernel_id,
    const TiRpmsg_Rpc_WireBufferDesc *inputs,
    uint16_t num_inputs,
    const TiRpmsg_Rpc_WireBufferDesc *outputs,
    uint16_t num_outputs,
    const void *params,
    uint32_t params_size,
    int32_t *remote_status);

#ifdef __cplusplus
}
#endif

#endif /* TI_RPMSG_RPC_CLIENT_H_ */
