#include "ti_rpmsg_rpc_client.h"
#include "rpmsg.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TI_RPMSG_RPC_MAX_MESSAGE_SIZE   (512U)

static int validate_response(const TiRpmsg_Rpc_WireHeader *response,
                             uint16_t expected_opcode,
                             uint32_t expected_sequence,
                             int response_length,
                             int32_t *remote_status)
{
    if ((response == NULL) || (remote_status == NULL)) {
        return -EINVAL;
    }

    if (response_length < (int)sizeof(*response)) {
        return -EPROTO;
    }

    if ((response->magic != TI_RPMSG_RPC_PROTOCOL_MAGIC) ||
        (response->version != TI_RPMSG_RPC_PROTOCOL_VERSION) ||
        (response->service != TI_RPMSG_RPC_SERVICE_GENERIC) ||
        (response->opcode != expected_opcode) ||
        (response->sequence != expected_sequence)) {
        return -EPROTO;
    }

    *remote_status = response->status;
    return 0;
}

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
    int32_t *remote_status)
{
    uint8_t request[TI_RPMSG_RPC_MAX_MESSAGE_SIZE];
    TiRpmsg_Rpc_WireHeader response;
    uint16_t opcode;
    size_t request_length;
    int response_length;
    uint32_t i;

    if ((rpmsg_fd < 0) || (remote_status == NULL) ||
        (num_inputs > TI_RPMSG_RPC_MAX_BUFFERS) ||
        (num_outputs > TI_RPMSG_RPC_MAX_BUFFERS) ||
        ((num_inputs != 0U) && (inputs == NULL)) ||
        ((num_outputs != 0U) && (outputs == NULL)) ||
        ((params_size != 0U) && (params == NULL))) {
        return -EINVAL;
    }

    memset(request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));

    if (params_size == 0U) {
        TiRpmsg_Rpc_GenericExecuteMessage *message =
            (TiRpmsg_Rpc_GenericExecuteMessage *)(void *)request;

        opcode = TI_RPMSG_RPC_GENERIC_OP_EXECUTE;
        request_length = sizeof(*message);

        message->header.magic = TI_RPMSG_RPC_PROTOCOL_MAGIC;
        message->header.version = TI_RPMSG_RPC_PROTOCOL_VERSION;
        message->header.service = TI_RPMSG_RPC_SERVICE_GENERIC;
        message->header.opcode = opcode;
        message->header.flags = 0U;
        message->header.sequence = sequence;
        message->header.status = 0;
        message->header.payload_size =
            (uint32_t)(request_length - sizeof(message->header));
        message->kernel_id = kernel_id;
        message->num_inputs = num_inputs;
        message->num_outputs = num_outputs;

        for (i = 0U; i < num_inputs; i++) {
            message->inputs[i] = inputs[i];
        }
        for (i = 0U; i < num_outputs; i++) {
            message->outputs[i] = outputs[i];
        }
    } else {
        TiRpmsg_Rpc_GenericExecuteParamsMessage *message =
            (TiRpmsg_Rpc_GenericExecuteParamsMessage *)(void *)request;

        opcode = TI_RPMSG_RPC_GENERIC_OP_EXECUTE_WITH_PARAMS;
        request_length = sizeof(*message) + (size_t)params_size;
        if (request_length > sizeof(request)) {
            return -EMSGSIZE;
        }

        message->header.magic = TI_RPMSG_RPC_PROTOCOL_MAGIC;
        message->header.version = TI_RPMSG_RPC_PROTOCOL_VERSION;
        message->header.service = TI_RPMSG_RPC_SERVICE_GENERIC;
        message->header.opcode = opcode;
        message->header.flags = 0U;
        message->header.sequence = sequence;
        message->header.status = 0;
        message->header.payload_size =
            (uint32_t)(request_length - sizeof(message->header));
        message->kernel_id = kernel_id;
        message->num_inputs = num_inputs;
        message->num_outputs = num_outputs;
        message->params_size = params_size;

        for (i = 0U; i < num_inputs; i++) {
            message->inputs[i] = inputs[i];
        }
        for (i = 0U; i < num_outputs; i++) {
            message->outputs[i] = outputs[i];
        }

        memcpy(request + sizeof(*message), params, params_size);
    }

    if (send_msg(rpmsg_fd, (char *)(void *)request, (int)request_length) < 0) {
        return -EIO;
    }

    response_length = (int)sizeof(response);
    if (recv_msg(rpmsg_fd,
                 (int)sizeof(response),
                 (char *)(void *)&response,
                 &response_length) < 0) {
        return -EIO;
    }

    return validate_response(&response,
                             opcode,
                             sequence,
                             response_length,
                             remote_status);
}
