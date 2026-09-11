#ifndef PIPELINE_COMMON_H
#define PIPELINE_COMMON_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <array>
#include <cerrno>

extern "C" {
#include "dmabuf.h"
}

class PipelineError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string hex_address(uint64_t address);

size_t require_param(const std::map<std::string, std::string>& params,
                     const char* key, const char* stage);

class DmaBuffer {
public:
    DmaBuffer(size_t bytes, std::string_view purpose);
    ~DmaBuffer();
    DmaBuffer(const DmaBuffer&) = delete;
    DmaBuffer& operator=(const DmaBuffer&) = delete;

    dma_buf_params* operator->() noexcept { return &params_; }
    const dma_buf_params* operator->() const noexcept { return &params_; }

    template <typename T>
    T* data() noexcept { return reinterpret_cast<T*>(params_.kern_addr); }

    void begin_cpu_access() const;
    void end_cpu_access() const;

private:
    void sync(int operation) const;

    dma_buf_params params_{};
    bool allocated_{false};
};

class AudioStream {
public:
    AudioStream() noexcept;
    ~AudioStream();
    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    void send_frame(uint8_t direction, const void* pcm, size_t bytes) noexcept;

private:
    static std::array<std::byte, 13> make_header(uint8_t direction,
                                                 uint32_t pcm_bytes) noexcept;
    static void write_u32_le(std::array<std::byte, 13>& destination,
                             size_t offset, uint32_t value) noexcept;
    void open() noexcept;
    bool send_all(const void* data, size_t bytes) noexcept;
    static void close_fd(int& descriptor) noexcept;

    static constexpr std::string_view socket_path_{"/tmp/edge-ai-speech.sock"};
    static_assert(socket_path_.size() < sizeof(sockaddr_un{}.sun_path),
                  "Audio stream socket path is too long");
    int server_{-1};
    int client_{-1};
};

#endif // PIPELINE_COMMON_H
