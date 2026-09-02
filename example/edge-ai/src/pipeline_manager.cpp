#include "pipeline_manager.h"
#include "pipeline_common.h"
#include "audio_utils.h"
#include "speech_enhancement_pipeline.h"
#include "audio_classification_pipeline.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <unistd.h>

extern "C" {
#include <json-c/json.h>
}

// ─── Model cache ──────────────────────────────────────────────────────────────

std::string PipelineManager::read_model_cache()
{
    std::ifstream f(MODEL_CACHE_FILE);
    if (!f.is_open())
        return {};
    std::string path;
    std::getline(f, path);
    return path;
}

bool PipelineManager::write_model_cache(const std::string& artifacts_path)
{
    std::filesystem::create_directories(
        std::filesystem::path(MODEL_CACHE_FILE).parent_path());
    std::ofstream f(MODEL_CACHE_FILE, std::ios::trunc);
    if (!f.is_open()) {
        std::cerr << "[App] Warning: cannot write model cache: " << MODEL_CACHE_FILE << std::endl;
        return false;
    }
    f << artifacts_path << '\n';
    return true;
}

// ─── Preload default model at boot ────────────────────────────────────────────

int PipelineManager::preload_default_model()
{
    if (!initialize())
        return -1;

    const std::string artifacts = DEFAULT_ARTIFACTS_PATH;
    std::cout << "[App] Preloading default model: " << artifacts << std::endl;

    const std::string cached = read_model_cache();
    if (cached == artifacts) {
        std::cout << "[App] Model already loaded (cache matches), nothing to do." << std::endl;
        return 0;
    }

    // Write cache so daemon loads correct artifacts on next restart
    if (!write_model_cache(artifacts))
        std::cerr << "[App] Warning: cannot write model cache: " << MODEL_CACHE_FILE << std::endl;

    tvm_client_ = std::make_shared<TvmInferenceClient>();
    if (!tvm_client_->initialize(artifacts)) {
        std::cerr << "[App] Failed to load default model from: " << artifacts << std::endl;
        return -1;
    }

    std::cout << "[App] Default model loaded and cached." << std::endl;
    return 0;
}

// ──────────────────────────────────────────────────────────────────────────────

PipelineManager::PipelineManager()
    : initialized_(false)
{
    state_.tvm_artifacts_configured = false;
    state_.input_configured = false;
}

PipelineManager::~PipelineManager()
{
}

bool PipelineManager::initialize()
{
    tvm_client_     = std::make_shared<TvmInferenceClient>();
    generic_client_ = std::make_unique<DspTaskClient>();
    initialized_ = true;
    return true;
}

int PipelineManager::run_from_json_file(const std::string& json_file_path)
{
    if (!initialize()) {
        std::cout << "[App] Failed to initialize application" << std::endl;
        return -1;
    }

    std::ifstream file(json_file_path);
    if (!file.is_open()) {
        std::cout << "[App] Error: JSON file not found: " << json_file_path << std::endl;
        return -1;
    }

    std::string json_content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    if (!loadPipelineFromJson(json_content)) {
        std::cout << "[App] Error: Failed to parse pipeline configuration" << std::endl;
        return -1;
    }

    state_.current_pipeline_file = json_file_path;

    std::cout << "[App] Pipeline type: " << state_.pipeline_config.pipeline_type << std::endl;
    std::cout << "[App] Description: "   << state_.pipeline_config.description   << std::endl;

    // Configure TVM artifacts if specified in JSON
    if (!state_.pipeline_config.artifacts_path.empty()) {
        const std::string& path = state_.pipeline_config.artifacts_path;
        if (!std::filesystem::exists(path)) {
            std::cout << "[App] Error: Artifacts path not found: " << path << std::endl;
            return -1;
        }
        state_.tvm_artifacts_paths = {path};
        state_.tvm_artifacts_configured = true;
        std::cout << "[App] TVM artifacts configured: " << path << std::endl;

        // Restart daemon if a different model is requested
        const std::string cached = read_model_cache();
        if (cached != path) {
            std::cout << "[App] Model changed (" << cached << " -> " << path
                      << "), restarting daemon..." << std::endl;
            write_model_cache(path);
            int ret = ::system("systemctl restart tvm-model-daemon");
            (void)ret;
            // Wait for daemon to be ready (socket appears)
            for (int i = 0; i < 60; ++i) {
                ::sleep(1);
                if (std::filesystem::exists("/var/run/tvm-inference.sock")) {
                    std::cout << "[App] Daemon ready." << std::endl;
                    break;
                }
            }
        }
    }

    // Configure input file
    if (state_.pipeline_config.input_file.empty()) {
        std::cout << "[App] Error: No input_file specified in pipeline JSON" << std::endl;
        return -1;
    }
    const std::string& input_file = state_.pipeline_config.input_file;
    if (!std::filesystem::exists(input_file)) {
        std::cout << "[App] Error: Input file not found: " << input_file << std::endl;
        return -1;
    }
    const auto extension = std::filesystem::path{input_file}.extension();
    if (extension == ".wav")
        state_.input_type = InputType::AUDIO_WAV;
    else if (extension == ".bin")
        state_.input_type = InputType::TENSOR_BIN;
    else {
        std::cout << "[App] Warning: Unknown input type for file: " << input_file << std::endl;
        state_.input_type = InputType::UNKNOWN;
    }
    state_.current_input_file = input_file;
    state_.input_configured = true;
    std::cout << "[App] Input configured: " << input_file << std::endl;

    if (!validateConfiguration())
        return -1;

    std::cout << "[App] Stages: " << state_.pipeline_config.stages.size() << std::endl;

    const auto& pipeline_type = state_.pipeline_config.pipeline_type;
    CommandResult result;

    if (pipeline_type == "speech_enhancement") {
        result = run_speech_enhancement_pipeline(state_, *generic_client_, *tvm_client_, debug_);
    } else if (pipeline_type == "audio_classification") {
        result = run_audio_classification_pipeline(state_, *generic_client_, *tvm_client_, debug_);
    } else {
        std::cout << "[App] Error: Unknown pipeline_type: " << pipeline_type << std::endl;
        return -1;
    }

    return (result == CommandResult::SUCCESS) ? 0 : 1;
}

bool PipelineManager::loadPipelineFromJson(const std::string& json_content)
{
    std::unique_ptr<json_object, decltype(&json_object_put)> root{
        json_tokener_parse(json_content.c_str()), &json_object_put};
    if (!root || !json_object_is_type(root.get(), json_type_object)) {
        std::cout << "[App] Error: Invalid JSON format" << std::endl;
        return false;
    }

    const auto read_string = [](json_object* object, const char* key,
                                std::string& destination, bool required) {
        json_object* value = nullptr;
        if (!json_object_object_get_ex(object, key, &value))
            return !required;
        if (!json_object_is_type(value, json_type_string))
            return false;
        destination = json_object_get_string(value);
        return !required || !destination.empty();
    };

    PipelineConfig config;
    if (!read_string(root.get(), "pipeline_type", config.pipeline_type, true) ||
        !read_string(root.get(), "description",   config.description,   false) ||
        !read_string(root.get(), "input_file",    config.input_file,    true) ||
        !read_string(root.get(), "artifacts_path",config.artifacts_path, false) ||
        !read_string(root.get(), "labels_path",   config.labels_path,   false)) {
        std::cout << "[App] Error: Missing or invalid pipeline field" << std::endl;
        return false;
    }

    // Optional num_classes (0 = derive from model output at runtime)
    {
        json_object* val = nullptr;
        if (json_object_object_get_ex(root.get(), "num_classes", &val) &&
            json_object_is_type(val, json_type_int))
            config.num_classes = static_cast<size_t>(json_object_get_int(val));
    }

    // Optional overlap_frames (-1 = not set, no overlap-save processing)
    {
        json_object* val = nullptr;
        if (json_object_object_get_ex(root.get(), "overlap_frames", &val) &&
            json_object_is_type(val, json_type_int))
            config.overlap_frames = json_object_get_int(val);
    }

    // Parse dsp_config — required for pipelines using DSP generic service
    json_object* dsp_cfg = nullptr;
    if (json_object_object_get_ex(root.get(), "dsp_config", &dsp_cfg) &&
        json_object_is_type(dsp_cfg, json_type_object)) {
        json_object* val = nullptr;
        if (json_object_object_get_ex(dsp_cfg, "proc_id", &val) &&
            json_object_is_type(val, json_type_int))
            config.dsp_config.proc_id = json_object_get_int(val);
        if (json_object_object_get_ex(dsp_cfg, "endpoint", &val) &&
            json_object_is_type(val, json_type_int))
            config.dsp_config.endpoint = json_object_get_int(val);
    }

    json_object* stages = nullptr;
    if (!json_object_object_get_ex(root.get(), "stages", &stages) ||
        !json_object_is_type(stages, json_type_array) ||
        json_object_array_length(stages) == 0) {
        std::cout << "[App] Error: Pipeline requires a non-empty stages array" << std::endl;
        return false;
    }

    const size_t stage_count = json_object_array_length(stages);
    config.stages.reserve(stage_count);
    for (size_t index = 0; index < stage_count; ++index) {
        json_object* object = json_object_array_get_idx(stages, index);
        PipelineStage stage;
        if (!object || !json_object_is_type(object, json_type_object) ||
            !read_string(object, "stage_id",     stage.stage_id,     true) ||
            !read_string(object, "service",      stage.service,      true) ||
            !read_string(object, "message_type", stage.message_type, true) ||
            (stage.service != "generic" && stage.service != "tvm")) {
            std::cout << "[App] Error: Invalid pipeline stage at index " << index << std::endl;
            return false;
        }

        json_object* parameters = nullptr;
        if (json_object_object_get_ex(object, "parameters", &parameters)) {
            if (!json_object_is_type(parameters, json_type_object)) {
                std::cout << "[App] Error: Stage parameters must be an object" << std::endl;
                return false;
            }
            json_object_object_foreach(parameters, key, value) {
                if (!json_object_is_type(value, json_type_string) &&
                    !json_object_is_type(value, json_type_int)) {
                    std::cout << "[App] Error: Stage parameter '" << key
                              << "' must be a string or integer" << std::endl;
                    return false;
                }
                stage.parameters.emplace(key, json_object_get_string(value));
            }
        }
        config.stages.push_back(std::move(stage));
    }

    config.loaded = true;
    state_.pipeline_config = std::move(config);
    return true;
}

bool PipelineManager::validateConfiguration()
{
    if (!state_.pipeline_config.loaded) {
        std::cout << "[App] Error: No pipeline configuration loaded" << std::endl;
        return false;
    }

    bool has_tvm_stages = false;
    for (const auto& stage : state_.pipeline_config.stages) {
        if (stage.service == "tvm") { has_tvm_stages = true; break; }
    }

    if (has_tvm_stages && !state_.tvm_artifacts_configured) {
        std::cout << "[App] Error: Pipeline has TVM stages but no TVM artifacts configured" << std::endl;
        return false;
    }

    if (!state_.input_configured) {
        std::cout << "[App] Error: No input data configured" << std::endl;
        return false;
    }

    return true;
}
