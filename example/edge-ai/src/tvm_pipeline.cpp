#include "tvm_pipeline.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <cstdint>

namespace {

bool saveTensorFile(const std::string& filename, const std::vector<float>& tensor_data)
{
    if (tensor_data.empty()) {
        std::cout << "[App] Error: Refusing to write an empty tensor" << std::endl;
        return false;
    }
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cout << "[App] Error: Cannot open file for writing: " << filename << std::endl;
        return false;
    }

    file.write(reinterpret_cast<const char*>(tensor_data.data()),
               static_cast<std::streamsize>(tensor_data.size() * sizeof(float)));
    if (!file) {
        std::cout << "[App] Error: Failed while writing tensor data" << std::endl;
        return false;
    }

    std::cout << "[App] Successfully saved " << tensor_data.size() << " float values ("
              << (tensor_data.size() * sizeof(float)) << " bytes) to " << filename << std::endl;

    return true;
}

} // namespace

PipelineManager::CommandResult run_tvm_pipeline(
    PipelineManager::State& state,
    TvmInferenceClient& tvm_client)
{
    std::cout << "\n[App] === Executing Tensor-Only Pipeline ===" << std::endl;

    if (state.pipeline_config.stages.size() != 1 ||
        state.pipeline_config.stages[0].service != "tvm") {
        std::cout << "[App] Error: Tensor pipeline must have exactly 1 TVM stage" << std::endl;
        return PipelineManager::CommandResult::ERROR;
    }

    std::cout << "[App] Running TVM inference" << std::endl;
    std::cout << "[App] Artifacts: " << state.tvm_artifacts_paths[0] << std::endl;
    std::cout << "[App] Input:     " << state.current_input_file << std::endl;

    if (!tvm_client.initialize(state.tvm_artifacts_paths[0])) {
        std::cout << "[App] Error: Failed to initialize TVM client" << std::endl;
        return PipelineManager::CommandResult::ERROR;
    }

    // Load input .bin as flat float32 tensor
    std::ifstream f(state.current_input_file, std::ios::binary | std::ios::ate);
    if (!f.is_open() || f.tellg() <= 0 ||
        f.tellg() % static_cast<std::streamoff>(sizeof(float)) != 0) {
        std::cout << "[App] Error: Cannot open or invalid input tensor: "
                  << state.current_input_file << std::endl;
        return PipelineManager::CommandResult::ERROR;
    }
    const size_t num_floats = static_cast<size_t>(f.tellg()) / sizeof(float);
    f.seekg(0);
    std::vector<float> input(num_floats), output;
    f.read(reinterpret_cast<char*>(input.data()), num_floats * sizeof(float));
    std::cout << "[App] Loaded " << num_floats << " floats from "
              << state.current_input_file << std::endl;

    if (!tvm_client.run_inference(input, output,
            std::vector<int64_t>{static_cast<int64_t>(num_floats)})) {
        std::cout << "[App] Error: TVM inference failed" << std::endl;
        return PipelineManager::CommandResult::ERROR;
    }

    const std::filesystem::path input_path{state.current_input_file};
    const std::string output_file =
        (input_path.parent_path() / (input_path.stem().string() + "_output.bin")).string();

    if (!saveTensorFile(output_file, output)) {
        std::cout << "[App] Error: Failed to save output tensor" << std::endl;
        return PipelineManager::CommandResult::ERROR;
    }

    std::cout << "[App] Output saved to: " << output_file << std::endl;
    std::cout << "[App] Pipeline completed successfully" << std::endl;
    std::cout << "PASSED" << std::endl;

    return PipelineManager::CommandResult::SUCCESS;
}
