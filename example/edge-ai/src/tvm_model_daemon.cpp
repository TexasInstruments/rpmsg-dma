/*
 * tvm_model_daemon.cpp — Persistent TVM model loader daemon
 *
 * Calls Module::LoadFromFile once at startup, then serves inference requests
 * from rpmsg_inference_example instances over a Unix domain socket.  Each demo
 * run connects, issues one INFER_REQ per chunk, then disconnects.  The model
 * stays resident for the lifetime of this process.
 *
 * Usage:
 *   tvm_model_daemon [--artifacts /path/to/artifacts/dir]
 *
 * Managed by tvm-model-daemon.service (started at boot, before the webserver).
 */

#include "tvm_inference_client.h"
#include "tvm_daemon_proto.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

static constexpr const char* DEFAULT_ARTIFACTS = "/usr/share/tvm_inference/artifacts/gcrn";

static constexpr const char* C7X_FW_LINK      = "/lib/firmware/am62d-c71_0-fw";
static constexpr const char* C7X_FW_TARGET    = "/lib/firmware/ti-ipc/am62dxx/dsp_edgeai.c75ss0-0.release.strip.out";
static constexpr const char* RPROC_STATE      = "/sys/class/remoteproc/remoteproc0/state";
static constexpr int         BOOT_TIMEOUT_S   = 60;
static constexpr int         STOP_TIMEOUT_S   = 1;

static std::string read_sysfs(const char* path) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return "";
    char buf[64] = {};
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return "";
    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
    return std::string(buf);
}

static bool write_sysfs(const char* path, const char* value) {
    int fd = ::open(path, O_WRONLY);
    if (fd < 0) return false;
    ssize_t n = ::write(fd, value, std::strlen(value));
    ::close(fd);
    return n > 0;
}

static bool fw_link_correct() {
    char target[512] = {};
    ssize_t n = ::readlink(C7X_FW_LINK, target, sizeof(target) - 1);
    if (n < 0) return false;
    target[n] = '\0';
    return std::strcmp(target, C7X_FW_TARGET) == 0;
}

/* Ensure C7x firmware symlink points to the edgeai binary and C7x is running.
 * If the symlink is wrong: stop C7x, fix symlink, start C7x.
 * If the symlink is correct but C7x is not running: start C7x.
 * Polls until running state or BOOT_TIMEOUT_S seconds. */
static bool wait_for_c7x() {
    bool need_restart = false;

    if (!fw_link_correct()) {
        std::cout << "[daemon] C7x firmware symlink incorrect — fixing\n";

        std::string state = read_sysfs(RPROC_STATE);
        if (state == "running" || state == "attached") {
            std::cout << "[daemon] Stopping C7x for firmware update...\n";
            if (!write_sysfs(RPROC_STATE, "stop")) {
                std::cerr << "[daemon] Failed to stop C7x: " << std::strerror(errno) << '\n';
                return false;
            }
            for (int i = 0; i < STOP_TIMEOUT_S; ++i) {
                ::sleep(1);
                if (read_sysfs(RPROC_STATE) == "offline") break;
            }
        }

        ::unlink(C7X_FW_LINK);
        if (::symlink(C7X_FW_TARGET, C7X_FW_LINK) != 0) {
            std::cerr << "[daemon] Failed to create firmware symlink: " << std::strerror(errno) << '\n';
            return false;
        }
        std::cout << "[daemon] Firmware symlink updated -> " << C7X_FW_TARGET << '\n';
        need_restart = true;
    }

    std::string state = read_sysfs(RPROC_STATE);
    if (state != "running") {
        std::cout << "[daemon] Starting C7x...\n";
        if (!write_sysfs(RPROC_STATE, "start")) {
            std::cerr << "[daemon] Failed to start C7x: " << std::strerror(errno) << '\n';
            return false;
        }
        need_restart = true;
    }

    if (!need_restart && state == "running") {
        std::cout << "[daemon] C7x firmware OK and already running\n";
        return true;
    }

    std::cout << "[daemon] Waiting for C7x to reach 'running' state (up to "
              << BOOT_TIMEOUT_S << "s)...\n";
    for (int i = 0; i < BOOT_TIMEOUT_S; ++i) {
        ::sleep(1);
        if (read_sysfs(RPROC_STATE) == "running") {
            std::cout << "[daemon] C7x is running\n";
            return true;
        }
    }
    std::cerr << "[daemon] Timeout: C7x did not reach 'running' state\n";
    return false;
}

namespace {

volatile sig_atomic_t g_running  = 1;
int                   g_server_fd = -1;

void signal_handler(int) {
    g_running = 0;
    if (g_server_fd >= 0) { ::close(g_server_fd); g_server_fd = -1; }
}

bool write_all(int fd, const void* buf, size_t len) {
    const auto* p = static_cast<const uint8_t*>(buf);
    while (len > 0) {
        ssize_t n = ::write(fd, p, len);
        if (n <= 0) return false;
        p += static_cast<size_t>(n); len -= static_cast<size_t>(n);
    }
    return true;
}

bool read_all(int fd, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    while (len > 0) {
        ssize_t n = ::read(fd, p, len);
        if (n <= 0) return false;
        p += static_cast<size_t>(n); len -= static_cast<size_t>(n);
    }
    return true;
}

bool send_hdr(int fd, TvmDaemon::MsgType type, uint32_t payload_len) {
    TvmDaemon::Header h{TvmDaemon::MAGIC, static_cast<uint32_t>(type), payload_len};
    return write_all(fd, &h, sizeof(h));
}

/* Serve a connected client until it disconnects or an error occurs.
 * Handles PING and repeated INFER_REQ messages on a single connection. */
void handle_client(int cfd, TvmInferenceClient& tvm) {
    TvmDaemon::Header hdr{};
    while (true) {
        if (!read_all(cfd, &hdr, sizeof(hdr))) break;
        if (hdr.magic != TvmDaemon::MAGIC) {
            std::cerr << "[daemon] Bad magic 0x" << std::hex << hdr.magic
                      << std::dec << " — closing\n";
            break;
        }

        if (hdr.type == static_cast<uint32_t>(TvmDaemon::MsgType::PING)) {
            send_hdr(cfd, TvmDaemon::MsgType::PONG, 0);
            continue;
        }

        if (hdr.type != static_cast<uint32_t>(TvmDaemon::MsgType::INFER_REQ) ||
            hdr.len == 0 || hdr.len % sizeof(float) != 0) {
            const std::string err = "unexpected message type or bad payload size";
            send_hdr(cfd, TvmDaemon::MsgType::ERROR_RESP,
                     static_cast<uint32_t>(err.size()));
            write_all(cfd, err.data(), err.size());
            break;
        }

        const size_t n_floats = hdr.len / sizeof(float);
        std::vector<float> input(n_floats), output;

        if (!read_all(cfd, input.data(), hdr.len)) {
            std::cerr << "[daemon] Failed to read input payload\n";
            break;
        }

        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = tvm.run_inference(input, output,
            std::vector<int64_t>{static_cast<int64_t>(n_floats)});
        const double ms = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count() / 1000.0;

        if (!ok) {
            const std::string err = "inference failed";
            send_hdr(cfd, TvmDaemon::MsgType::ERROR_RESP,
                     static_cast<uint32_t>(err.size()));
            write_all(cfd, err.data(), err.size());
            continue;
        }

        std::cout << "[daemon] Inference " << ms << " ms ("
                  << output.size() << " floats out)\n";

        const uint32_t out_bytes = static_cast<uint32_t>(output.size() * sizeof(float));
        if (!send_hdr(cfd, TvmDaemon::MsgType::INFER_RESP, out_bytes) ||
            !write_all(cfd, output.data(), out_bytes)) {
            std::cerr << "[daemon] Failed to send response\n";
            break;
        }
    }
    ::close(cfd);
}

} // namespace

static constexpr const char* MODEL_CACHE_FILE = "/var/lib/tvm_inference/loaded_model";

static std::string read_model_cache() {
    int fd = ::open(MODEL_CACHE_FILE, O_RDONLY);
    if (fd < 0) return {};
    char buf[512] = {};
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return {};
    std::string s(buf, static_cast<size_t>(n));
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

int main(int argc, char* argv[]) {
    // Priority: --artifacts arg > cache file > hardcoded default
    std::string artifacts;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--artifacts" && i + 1 < argc)
            artifacts = argv[++i];
    }
    if (artifacts.empty()) artifacts = read_model_cache();
    if (artifacts.empty()) artifacts = DEFAULT_ARTIFACTS;

    std::cout << "[daemon] Loading TVM artifacts from: " << artifacts << '\n';

    if (!wait_for_c7x()) {
        std::cerr << "[daemon] C7x not ready — aborting\n";
        return 1;
    }

    TvmInferenceClient tvm;
    tvm.disable_daemon();               /* must not try to connect to itself */

    if (!tvm.initialize(artifacts)) {
        std::cerr << "[daemon] Failed to load model — aborting\n";
        return 1;
    }
    std::cout << "[daemon] Model ready. Listening on "
              << TvmDaemon::SOCKET_PATH << '\n';

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);   /* suppress broken-pipe crashes */

    ::unlink(TvmDaemon::SOCKET_PATH);
    g_server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0) { perror("socket"); return 1; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, TvmDaemon::SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (::bind(g_server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    ::chmod(TvmDaemon::SOCKET_PATH, 0660);

    if (::listen(g_server_fd, 4) < 0) { perror("listen"); return 1; }
    std::cout << "[daemon] Ready\n";

    while (g_running) {
        int cfd = ::accept(g_server_fd, nullptr, nullptr);
        if (cfd < 0) {
            if (g_running) perror("accept");
            break;
        }
        handle_client(cfd, tvm);
    }

    ::unlink(TvmDaemon::SOCKET_PATH);
    std::cout << "[daemon] Shut down\n";
    return 0;
}
