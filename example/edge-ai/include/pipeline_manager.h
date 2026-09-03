#ifndef PIPELINE_MANAGER_H
#define PIPELINE_MANAGER_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include "tvm_inference_client.h"
#include "dsp_task_client.h"

extern "C" {
#include <json-c/json.h>
#include "dmabuf.h"
}

class PipelineManager {
public:
    enum class CommandResult {
        SUCCESS,
        ERROR,
        QUIT
    };

    struct PipelineStage {
        std::string stage_id;
        std::string service;
        std::string message_type;
        std::map<std::string, std::string> parameters;
    };

    struct DspConfig {
        int proc_id  = 0;
        int endpoint = 0;
    };

    struct PipelineConfig {
        std::string pipeline_type;
        std::string description;
        std::string input_file;
        std::string artifacts_path;
        std::string labels_path;      // optional — classification pipelines only
        size_t      num_classes{0};   // optional — 0 = derive from model output
        int         overlap_frames{-1}; // optional — enhancement only; -1 = no overlap
        std::vector<PipelineStage> stages;
        DspConfig dsp_config;
        bool loaded;

        PipelineConfig() : loaded(false) {}
    };

    enum class InputType {
        UNKNOWN,
        AUDIO_WAV,
        TENSOR_BIN
    };

    struct State {
        PipelineConfig pipeline_config;
        std::string current_pipeline_file;
        std::vector<std::string> tvm_artifacts_paths;
        bool tvm_artifacts_configured;
        std::string current_input_file;
        InputType input_type;
        bool input_configured;

        State() : input_type(InputType::UNKNOWN) {}
    };

    // Path where the currently-loaded model artifacts path is persisted across runs
    static constexpr const char* MODEL_CACHE_FILE = "/var/lib/tvm_inference/loaded_model";
    // Default artifacts loaded at boot via --preload
    static constexpr const char* DEFAULT_ARTIFACTS_PATH = "/usr/share/tvm_inference/artifacts/gcrn";

    PipelineManager();
    ~PipelineManager();
    bool initialize();
    int run_from_json_file(const std::string& json_file_path);
    int run_from_json_file_stream(const std::string& json_file_path);
    int preload_default_model();
    void set_debug(bool enable) { debug_ = enable; }

private:
    std::shared_ptr<TvmInferenceClient> tvm_client_;
    std::unique_ptr<DspTaskClient> generic_client_;
    State state_;
    bool initialized_;
    bool debug_ = false;

    bool validateConfiguration();
    bool loadPipelineFromJson(const std::string& json_content);

    // Model cache: read/write the artifacts path persisted on disk
    static std::string read_model_cache();
    static bool write_model_cache(const std::string& artifacts_path);
};

#endif // PIPELINE_MANAGER_H
