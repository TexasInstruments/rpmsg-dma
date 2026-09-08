#ifndef TVM_INFERENCE_CLIENT_H
#define TVM_INFERENCE_CLIENT_H

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// Forward declarations to avoid including TVM headers here
namespace tvm {
namespace runtime {
class Module;
class PackedFunc;
}
}

// Forward declaration — full definition in tvm_daemon_proto.h (included by .cpp)
namespace TvmDaemon { struct Header; }

class TvmInferenceClient {
private:
    std::string artifacts_path_;
    std::unique_ptr<tvm::runtime::Module> graph_executor_;
    std::unique_ptr<tvm::runtime::PackedFunc> set_input_;
    std::unique_ptr<tvm::runtime::PackedFunc> run_;
    std::unique_ptr<tvm::runtime::PackedFunc> get_output_;

    bool initialized_;


    // Daemon client state
    int  daemon_fd_{-1};       /* Unix socket fd when using daemon; -1 = local mode */
    bool daemon_skip_{false};  /* true = never try daemon (set by daemon itself)     */

public:
    TvmInferenceClient();
    ~TvmInferenceClient();

    // Initialization
    bool initialize(const std::string& artifacts_path);
    void cleanup();

    // Inference
    bool run_inference(const std::vector<float>& input,
                       std::vector<float>& output,
                       const std::vector<int64_t>& shape);

    // Status
    bool is_initialized() const { return initialized_; }

    /* Prevent daemon auto-connect — must be called before initialize().
     * Used by tvm_model_daemon to avoid recursion into itself. */
    void disable_daemon() noexcept { daemon_skip_ = true; }
    bool is_daemon_mode() const noexcept { return daemon_fd_ >= 0; }

    /* Request the running daemon to hot-reload a different model.
     * Opens a one-shot connection — no root or systemctl required.
     * Returns true when the daemon confirms the new model is loaded.
     * Returns false if the daemon is unreachable or rejects the path. */
    static bool switch_model(const std::string& artifacts_path);

private:
    // Helper methods
    bool load_artifacts();
    std::string load_json_file(const std::string& path);

    // Daemon client helpers
    bool try_daemon_connect();
    bool run_via_daemon(const float* input, size_t count, std::vector<float>& output);
};

#endif // TVM_INFERENCE_CLIENT_H
