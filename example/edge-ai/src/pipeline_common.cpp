#include "pipeline_common.h"
#include <sstream>

std::string hex_address(uint64_t address)
{
    std::ostringstream value;
    value << "0x" << std::hex << address;
    return value.str();
}

DmaBuffer::DmaBuffer(size_t bytes, std::string_view purpose)
{
    if (bytes > std::numeric_limits<uint32_t>::max())
        throw PipelineError{"DMA allocation is larger than the API limit"};
    char heap[] = "linux,cma";
    char remoteproc[] = "/dev/remoteproc0";
    if (dmabuf_heap_init(heap, static_cast<uint32_t>(bytes), remoteproc, &params_) != 0)
        throw PipelineError{"Failed to allocate DMA buffer for " + std::string{purpose}};
    allocated_ = true;
}

DmaBuffer::~DmaBuffer()
{
    if (allocated_) dmabuf_heap_destroy(&params_);
}

void DmaBuffer::begin_cpu_access() const { sync(DMA_BUF_SYNC_START); }
void DmaBuffer::end_cpu_access() const   { sync(DMA_BUF_SYNC_END); }

void DmaBuffer::sync(int operation) const
{
    if (dmabuf_sync(params_.dma_buf_fd, operation) != 0)
        throw PipelineError{"DMA buffer synchronization failed"};
}

size_t require_param(const std::map<std::string, std::string>& params,
                     const char* key, const char* stage)
{
    auto it = params.find(key);
    if (it == params.end())
        throw PipelineError{std::string{"Stage missing required parameter: "} + key +
                            " (stage: " + stage + ")"};
    int v = std::stoi(it->second);
    if (v <= 0)
        throw PipelineError{std::string{"Parameter must be positive: "} + key};
    return static_cast<size_t>(v);
}

AudioStream::AudioStream() noexcept { open(); }

AudioStream::~AudioStream()
{
    close_fd(client_);
    close_fd(server_);
    ::unlink(socket_path_.data());
}

void AudioStream::send_frame(uint8_t direction, const void* pcm, size_t bytes) noexcept
{
    if (server_ < 0 || !pcm || bytes > std::numeric_limits<uint32_t>::max())
        return;
    if (client_ < 0)
        client_ = ::accept(server_, nullptr, nullptr);
    if (client_ < 0)
        return;
    const auto header = make_header(direction, static_cast<uint32_t>(bytes));
    if (!send_all(header.data(), header.size()) || !send_all(pcm, bytes))
        close_fd(client_);
}

std::array<std::byte, 13> AudioStream::make_header(uint8_t direction,
                                                    uint32_t pcm_bytes) noexcept
{
    std::array<std::byte, 13> header{};
    header[0] = std::byte{'E'};
    header[1] = std::byte{'A'};
    header[2] = std::byte{'S'};
    header[3] = std::byte{'P'};
    header[4] = static_cast<std::byte>(direction);
    write_u32_le(header, 5, 16000);
    write_u32_le(header, 9, pcm_bytes);
    return header;
}

void AudioStream::write_u32_le(std::array<std::byte, 13>& destination,
                                size_t offset, uint32_t value) noexcept
{
    for (size_t index = 0; index < sizeof(value); ++index)
        destination[offset + index] =
            static_cast<std::byte>((value >> (index * 8)) & 0xffU);
}

void AudioStream::open() noexcept
{
    ::unlink(socket_path_.data());
    server_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (server_ < 0)
        return;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::copy(socket_path_.begin(), socket_path_.end(), address.sun_path);
    if (::bind(server_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(server_, 1) != 0)
        close_fd(server_);
}

bool AudioStream::send_all(const void* data, size_t bytes) noexcept
{
    const auto* cursor = static_cast<const std::byte*>(data);
    size_t sent = 0;
    while (sent < bytes) {
        const auto count = ::send(client_, cursor + sent, bytes - sent, MSG_NOSIGNAL);
        if (count > 0) {
            sent += static_cast<size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

void AudioStream::close_fd(int& descriptor) noexcept
{
    if (descriptor >= 0)
        ::close(descriptor);
    descriptor = -1;
}
