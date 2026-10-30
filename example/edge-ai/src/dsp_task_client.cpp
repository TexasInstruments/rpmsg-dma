#include "dsp_task_client.h"
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

extern "C" {
#include "rpmsg.h"
#include "dmabuf.h"
#include "ti_rpmsg_rpc_client.h"
}

namespace {

/* Must match examples/edge-ai/tvm/common/dsp_service.h in MCU+ SDK. */
constexpr uint32_t EDGEAI_DSP_KERNEL_STFT  = 1U;
constexpr uint32_t EDGEAI_DSP_KERNEL_ISTFT = 2U;
constexpr uint32_t EDGEAI_DSP_KERNEL_UTILS = 3U;

struct EdgeAiDspStftParams {
    uint32_t selected_model;
    uint32_t input_frame;
    uint32_t output_frame;
};

struct EdgeAiDspUtilsParams {
    uint32_t input_frame;
    uint32_t fft_size;
    uint32_t flag;
};

static_assert(sizeof(EdgeAiDspStftParams) == 12U);
static_assert(sizeof(EdgeAiDspUtilsParams) == 12U);

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

uint64_t parameter_address(const std::map<std::string, std::string>& parameters,
                           const std::string& name)
{
    const auto parameter = parameters.find(name);
    if (parameter == parameters.end())
        return 0U;

    size_t parsed = 0;
    const auto value = std::stoull(parameter->second, &parsed, 16);
    if (parsed != parameter->second.size())
        throw std::out_of_range{name + " is not a uint64 address"};
    return static_cast<uint64_t>(value);
}

uint32_t wire_size(uint64_t bytes, const char *what)
{
    if (bytes > std::numeric_limits<uint32_t>::max())
        throw std::out_of_range{std::string{what} + " exceeds TI-Rpmsg-Rpc wire size"};
    return static_cast<uint32_t>(bytes);
}

std::string client_error(int status)
{
    if (status >= 0)
        return "unknown error";
    return std::strerror(-status);
}

bool run_generic_kernel(int rpmsg_fd,
                        uint32_t sequence,
                        uint32_t kernel_id,
                        uint64_t input_address,
                        uint32_t input_size,
                        uint64_t output_address,
                        uint32_t output_size,
                        const void *params,
                        uint32_t params_size,
                        int32_t& remote_status,
                        std::string& error)
{
    TiRpmsg_Rpc_WireBufferDesc input = {};
    TiRpmsg_Rpc_WireBufferDesc output = {};

    input.address = input_address;
    input.size = input_size;
    input.flags = 0U;

    output.address = output_address;
    output.size = output_size;
    output.flags = 0U;

    const int status = ti_rpmsg_rpc_generic_execute(rpmsg_fd,
                                                   sequence,
                                                   kernel_id,
                                                   &input,
                                                   1U,
                                                   &output,
                                                   1U,
                                                   params,
                                                   params_size,
                                                   &remote_status);
    if (status != 0) {
        error = "TI-Rpmsg-Rpc exchange failed: " + client_error(status) +
                " (" + std::to_string(status) + ")";
        return false;
    }

    if (remote_status != 0) {
        error = "TI-Rpmsg-Rpc kernel failed, remote status=" +
                std::to_string(remote_status);
        return false;
    }

    return true;
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
        if ((message_type == "C7X_MSG_STFT_ANALYZE") ||
            (message_type == "C7X_MSG_ISTFT_SYNTHESIZE")) {
            const bool is_stft = (message_type == "C7X_MSG_STFT_ANALYZE");
            const uint32_t kernel_id = is_stft ? EDGEAI_DSP_KERNEL_STFT
                                               : EDGEAI_DSP_KERNEL_ISTFT;
            const uint32_t input_frame = parameter_value(parameters, "input_frame", 0U);
            const uint32_t output_frame = parameter_value(parameters, "output_frame", 0U);
            const uint32_t hop_size = parameter_value(parameters, "hop_size", 0U);
            const uint32_t model_elems = parameter_value(parameters, "model_elems", 0U);
            const uint64_t input_address = parameter_address(parameters, "input_buffer");
            const uint64_t output_address = parameter_address(parameters, "output_buffer");
            EdgeAiDspStftParams params = {};
            uint32_t input_size;
            uint32_t output_size;
            int32_t remote_status = 0;
            std::string error;
            const uint32_t sequence = sequence_number_++;

            params.selected_model = parameter_value(parameters, "selected_model", 0U);
            params.input_frame = input_frame;
            params.output_frame = output_frame;

            if (is_stft) {
                input_size = wire_size((uint64_t)input_frame * hop_size * sizeof(int16_t),
                                       "STFT input");
                output_size = wire_size((uint64_t)output_frame * model_elems * sizeof(float),
                                        "STFT output");
            } else {
                input_size = wire_size((uint64_t)input_frame * model_elems * sizeof(float),
                                       "ISTFT input");
                output_size = wire_size((uint64_t)output_frame * hop_size * sizeof(int16_t),
                                        "ISTFT output");
            }

#ifdef DEBUG
            std::cout << "[GenericClient] TI-Rpmsg-Rpc "
                      << (is_stft ? "STFT" : "ISTFT")
                      << " kernel=" << kernel_id
                      << " seq=" << sequence
                      << " in=0x" << std::hex << input_address
                      << "/" << std::dec << input_size
                      << " out=0x" << std::hex << output_address
                      << "/" << std::dec << output_size
                      << " frames=" << input_frame << "->" << output_frame
                      << std::endl;
#endif

            if (!run_generic_kernel(rpmsg_fd_,
                                    sequence,
                                    kernel_id,
                                    input_address,
                                    input_size,
                                    output_address,
                                    output_size,
                                    &params,
                                    sizeof(params),
                                    remote_status,
                                    error)) {
                result.error_message = (is_stft ? "DSP STFT failed: "
                                                : "DSP ISTFT failed: ") + error;
                return result;
            }

            result.success = true;
            result.input_size = input_frame;
            result.output_size = output_frame;

        } else if (message_type == "C7X_DEINTERLEAVE_MSG_ANALYZE") {
            const uint32_t input_frame = parameter_value(parameters, "input_frame", 0U);
            const uint32_t fft_size = parameter_value(parameters, "fft_size", 0U);
            const uint32_t flag = parameter_value(parameters, "flag", 0U);
            const uint64_t input_address = parameter_address(parameters, "input_buffer");
            const uint64_t output_address = parameter_address(parameters, "output_buffer");
            const uint64_t bins = (uint64_t)(fft_size / 2U) + 1U;
            EdgeAiDspUtilsParams params = {};
            uint32_t buffer_size;
            int32_t remote_status = 0;
            std::string error;
            const uint32_t sequence = sequence_number_++;

            params.input_frame = input_frame;
            params.fft_size = fft_size;
            params.flag = flag;

            if ((flag == 0U) || (flag == 1U)) {
                buffer_size = wire_size(2ULL * input_frame * bins * sizeof(float),
                                        "deinterleave/interleave buffer");
            } else if ((flag == 2U) || (flag == 3U)) {
                buffer_size = wire_size((uint64_t)input_frame * bins * sizeof(double),
                                        "matrix-transpose buffer");
            } else {
                result.error_message = "Invalid utils flag: " + std::to_string(flag);
                return result;
            }

#ifdef DEBUG
            std::cout << "[GenericClient] TI-Rpmsg-Rpc utils kernel="
                      << EDGEAI_DSP_KERNEL_UTILS
                      << " seq=" << sequence
                      << " flag=" << flag
                      << " in=0x" << std::hex << input_address
                      << " out=0x" << output_address
                      << std::dec << " bytes=" << buffer_size
                      << std::endl;
#endif

            if (!run_generic_kernel(rpmsg_fd_,
                                    sequence,
                                    EDGEAI_DSP_KERNEL_UTILS,
                                    input_address,
                                    buffer_size,
                                    output_address,
                                    buffer_size,
                                    &params,
                                    sizeof(params),
                                    remote_status,
                                    error)) {
                result.error_message = "DSP layout conversion failed: " + error;
                return result;
            }

            result.success = true;
            result.input_size = input_frame;
            result.output_size = input_frame;

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
