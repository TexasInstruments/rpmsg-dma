#include "dsp_task_client.h"
#include <unistd.h>
#include <iostream>
#include <limits>
#include <stdexcept>

extern "C" {
#include "rpmsg.h"
#include "dmabuf.h"
}

namespace {


uint32_t parameter_value(const std::map<std::string, std::string>& parameters,
                         const std::string& name, uint32_t default_value,
                         int base = 10)
{
    const auto parameter = parameters.find(name);
    if (parameter == parameters.end())
        return default_value;

    size_t parsed = 0;
    const auto value = std::stoull(parameter->second, &parsed, base);
    if (parsed != parameter->second.size() ||
        value > std::numeric_limits<uint32_t>::max())
        throw std::out_of_range{name + " is not a uint32 value"};
    return static_cast<uint32_t>(value);
}

struct c7x_msg_hdr {
    uint32_t type;
    uint32_t seq;
    uint32_t len;
    int32_t  status;
};

/*
 * Generic DSP message — flat payload, 6 x uint32_t after the header.
 * Wire size is always fixed: sizeof(c7x_msg_hdr) + 6 * sizeof(uint32_t).
 *
 * Field mapping by message type:
 *
 *  param  | C7X_MSG_STFT_ANALYZE      | C7X_MSG_ISTFT_SYNTHESIZE  | C7X_DEINTERLEAVE_MSG_ANALYZE
 *  -------|---------------------------|---------------------------|-----------------------------
 *  param0 | selected_model            | selected_model            | input_buffer  (phys addr)
 *  param1 | input_buffer  (phys addr) | input_buffer  (phys addr) | output_buffer (phys addr)
 *  param2 | output_buffer (phys addr) | output_buffer (phys addr) | input_frame
 *  param3 | input_frame               | input_frame               | fft_size
 *  param4 | output_frame              | output_frame              | flag (0=deinterleave, 1=interleave)
 *  param5 | clear (0/1)               | clear (0/1)               | clear (0)
 *
 * Note: param0 maps differently per message type because the firmware
 * STFT/ISTFT and utils structs have different layouts (see table above).
 * param5 (clear) tells STFT to clear its graph when 1 (used for YAMNet/VGGish chunk boundaries).
 *
 * To add a new message type:
 *   1. Add its opcodes to c7x_msg_type below
 *   2. Add a column to this table
 *   3. Add a new else-if branch in DspTaskClient::process()
 *   4. Set unused params to 0
 */
struct dsp_msg {
    struct c7x_msg_hdr hdr;
    uint32_t param0;
    uint32_t param1;
    uint32_t param2;
    uint32_t param3;
    uint32_t param4;
    uint32_t param5;
};

enum c7x_msg_type {
    C7X_MSG_STFT_ANALYZE              = 0x1020,
    C7X_MSG_STFT_ANALYZE_RESP         = 0x2020,
    C7X_MSG_ISTFT_SYNTHESIZE          = 0x1030,
    C7X_MSG_ISTFT_SYNTHESIZE_RESP     = 0x2030,
    C7X_DEINTERLEAVE_MSG_ANALYZE      = 0x1040,
    C7X_DEINTERLEAVE_MSG_ANALYZE_RESP = 0x2040
};

static_assert(sizeof(c7x_msg_hdr) == 16);
static_assert(sizeof(dsp_msg) == sizeof(c7x_msg_hdr) + 6 * sizeof(uint32_t));

enum c7x_status {
    C7X_STATUS_SUCCESS = 0,
    C7X_STATUS_ERROR = -1
};

template <typename Message>
bool exchange_message(int descriptor, Message& request, Message& response)
{
    if (send_msg(descriptor, reinterpret_cast<char*>(&request), sizeof(request)) < 0)
        return false;

    int response_length = sizeof(response);
    if (recv_msg(descriptor, sizeof(response), reinterpret_cast<char*>(&response),
                 &response_length) < 0)
        return false;
    return response_length >= static_cast<int>(sizeof(c7x_msg_hdr));
}

} // namespace

DspTaskClient::DspTaskClient()
    : rpmsg_fd_(-1), proc_id_(0), endpoint_(0), initialized_(false), sequence_number_(1)
{
}

DspTaskClient::~DspTaskClient()
{
    shutdown();
}

bool DspTaskClient::initialize(int proc_id, int endpoint)
{
    if (initialized_) {
        return true;
    }

    proc_id_  = proc_id;
    endpoint_ = endpoint;

    if (!open_rpmsg_device()) {
        return false;
    }

    initialized_ = true;
    return true;
}

bool DspTaskClient::open_rpmsg_device()
{
    rpmsg_fd_ = init_rpmsg(proc_id_, endpoint_);
    if (rpmsg_fd_ < 0) {
        return false;
    }
    return true;
}

void DspTaskClient::close_rpmsg_device()
{
    if (rpmsg_fd_ >= 0) {
        ::close(rpmsg_fd_);
        rpmsg_fd_ = -1;
    }
}

DspTaskClient::ProcessingResult DspTaskClient::process(
    const std::string& message_type,
    const std::map<std::string, std::string>& parameters)
{
    ProcessingResult result = {};
    result.success = false;

    if (!initialized_) {
        result.error_message = "Client not initialized";
        return result;
    }

    try {
    // Determine message type and send appropriate struct
    if (message_type == "C7X_MSG_STFT_ANALYZE") {
        struct dsp_msg req = {};
        req.hdr.type   = C7X_MSG_STFT_ANALYZE;
        req.hdr.seq    = sequence_number_++;
        req.hdr.len    = sizeof(struct dsp_msg);
        req.hdr.status = 0;

        req.param0 = parameter_value(parameters, "selected_model", 0);     /* selected_model */
        req.param1 = parameter_value(parameters, "input_buffer",  0, 16);  /* input_buffer   */
        req.param2 = parameter_value(parameters, "output_buffer", 0, 16);  /* output_buffer  */
        req.param3 = parameter_value(parameters, "input_frame",   0);      /* input_frame    */
        req.param4 = parameter_value(parameters, "output_frame",  0);      /* output_frame   */
        req.param5 = parameter_value(parameters, "clear",         0);      /* clear flag     */
#ifdef DEBUG
        std::cout << "[GenericClient] STFT_ANALYZE - Sending to firmware:" << std::endl;
        std::cout << "[GenericClient]   selected_model=" << req.param0 << std::endl;
        std::cout << "[GenericClient]   input_buffer=0x"  << std::hex << req.param1 << std::endl;
        std::cout << "[GenericClient]   output_buffer=0x" << std::hex << req.param2 << std::endl;
        std::cout << "[GenericClient]   input_frame="  << std::dec << req.param3 << " frames" << std::endl;
        std::cout << "[GenericClient]   output_frame=" << std::dec << req.param4 << " frames" << std::endl;
        std::cout << "[GenericClient]   clear=" << req.param5 << std::endl;
#endif
        struct dsp_msg resp = {};
        if (!exchange_message(rpmsg_fd_, req, resp)) {
            result.error_message = "STFT analyze message exchange failed";
            return result;
        }

        if (resp.hdr.type != C7X_MSG_STFT_ANALYZE_RESP || resp.hdr.seq != req.hdr.seq) {
            result.error_message = "Invalid STFT analyze response";
            return result;
        }
#ifdef DEBUG
        std::cout << "[GenericClient] STFT_ANALYZE - Firmware responded:" << std::endl;
        std::cout << "[GenericClient]   status="       << resp.hdr.status << std::endl;
        std::cout << "[GenericClient]   input_frame="  << resp.param3 << " frames" << std::endl;
        std::cout << "[GenericClient]   output_frame=" << resp.param4 << " frames" << std::endl;
#endif
        if (resp.hdr.status != C7X_STATUS_SUCCESS) {
            result.error_message = "DSP STFT analyze failed";
            return result;
        }

        result.success = true;
        result.input_size  = resp.param3;   /* input_frame  */
        result.output_size = resp.param4;   /* output_frame */

    } else if (message_type == "C7X_MSG_ISTFT_SYNTHESIZE") {
        struct dsp_msg req = {};
        req.hdr.type   = C7X_MSG_ISTFT_SYNTHESIZE;
        req.hdr.seq    = sequence_number_++;
        req.hdr.len    = sizeof(struct dsp_msg);
        req.hdr.status = 0;

        req.param0 = parameter_value(parameters, "selected_model", 0);     /* selected_model */
        req.param1 = parameter_value(parameters, "input_buffer",  0, 16);  /* input_buffer   */
        req.param2 = parameter_value(parameters, "output_buffer", 0, 16);  /* output_buffer  */
        req.param3 = parameter_value(parameters, "input_frame",   0);      /* input_frame    */
        req.param4 = parameter_value(parameters, "output_frame",  0);      /* output_frame   */
        req.param5 = parameter_value(parameters, "clear",         0);      /* clear flag     */
#ifdef DEBUG
        std::cout << "[GenericClient] ISTFT_SYNTHESIZE - Sending to firmware:" << std::endl;
        std::cout << "[GenericClient]   selected_model=" << req.param0 << std::endl;
        std::cout << "[GenericClient]   input_buffer=0x"  << std::hex << req.param1 << std::endl;
        std::cout << "[GenericClient]   output_buffer=0x" << std::hex << req.param2 << std::endl;
        std::cout << "[GenericClient]   input_frame="  << std::dec << req.param3 << " frames" << std::endl;
        std::cout << "[GenericClient]   output_frame=" << std::dec << req.param4 << " frames" << std::endl;
        std::cout << "[GenericClient]   clear=" << req.param5 << std::endl;
#endif
        struct dsp_msg resp = {};
        if (!exchange_message(rpmsg_fd_, req, resp)) {
            result.error_message = "ISTFT synthesize message exchange failed";
            return result;
        }

        if (resp.hdr.type != C7X_MSG_ISTFT_SYNTHESIZE_RESP || resp.hdr.seq != req.hdr.seq) {
            result.error_message = "Invalid ISTFT synthesize response";
            return result;
        }
#ifdef DEBUG
        std::cout << "[GenericClient] ISTFT_SYNTHESIZE - Firmware responded:" << std::endl;
        std::cout << "[GenericClient]   status="       << resp.hdr.status << std::endl;
        std::cout << "[GenericClient]   input_frame="  << resp.param3 << " frames" << std::endl;
        std::cout << "[GenericClient]   output_frame=" << resp.param4 << " frames" << std::endl;
#endif
        if (resp.hdr.status != C7X_STATUS_SUCCESS) {
            result.error_message = "DSP ISTFT synthesize failed";
            return result;
        }

        result.success = true;
        result.input_size  = resp.param3;   /* input_frame  */
        result.output_size = resp.param4;   /* output_frame */

    } else if (message_type == "C7X_DEINTERLEAVE_MSG_ANALYZE") {
        struct dsp_msg req = {};
        req.hdr.type   = C7X_DEINTERLEAVE_MSG_ANALYZE;
        req.hdr.seq    = sequence_number_++;
        req.hdr.len    = sizeof(struct dsp_msg);
        req.hdr.status = 0;

        req.param0 = parameter_value(parameters, "input_buffer",  0, 16);   /* input_buffer   */
        req.param1 = parameter_value(parameters, "output_buffer", 0, 16);   /* output_buffer  */
        req.param2 = parameter_value(parameters, "input_frame",   0);       /* input_frame    */
        req.param3 = parameter_value(parameters, "fft_size",      0);       /* fft_size       */
        req.param4 = parameter_value(parameters, "flag",          0);       /* flag           */
        req.param5 = 0;                                                      /* clear (unused) */

        struct dsp_msg resp = {};
        if (!exchange_message(rpmsg_fd_, req, resp)) {
            result.error_message = "Layout conversion message exchange failed";
            return result;
        }

        if (resp.hdr.type != C7X_DEINTERLEAVE_MSG_ANALYZE_RESP || resp.hdr.seq != req.hdr.seq) {
            result.error_message = "Invalid layout conversion response";
            return result;
        }

        if (resp.hdr.status != C7X_STATUS_SUCCESS) {
            result.error_message = "DSP deinterleave/interleave failed";
            return result;
        }

        result.success = true;
        result.input_size  = resp.param2;   /* input_frame */
        result.output_size = resp.param2;   /* input_frame */

    } else {
        result.error_message = "Unknown message type: " + message_type;
        return result;
    }
    } catch (const std::exception& error) {
        result.error_message = "Invalid DSP stage parameter: " + std::string{error.what()};
        return result;
    }

    return result;
}

void DspTaskClient::shutdown()
{
    if (initialized_) {
        close_rpmsg_device();
        initialized_ = false;
    }
}
