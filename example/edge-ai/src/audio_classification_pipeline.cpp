#include "audio_classification_pipeline.h"
#include "pipeline_common.h"
#include "audio_utils.h"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

std::vector<std::string> load_labels(const std::string& path)
{
    std::vector<std::string> labels;
    if (path.empty()) return labels;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cout << "[App] Warning: labels file not found: " << path << std::endl;
        return labels;
    }
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        labels.push_back(line);
    }
    std::cout << "[App] Loaded " << labels.size() << " class labels from " << path << std::endl;
    return labels;
}

} // namespace

PipelineManager::CommandResult run_audio_classification_pipeline(
    PipelineManager::State& state,
    DspTaskClient& dsp_client,
    TvmInferenceClient& tvm_client,
    bool debug)
{
    try {
        if (state.input_type != PipelineManager::InputType::AUDIO_WAV)
            throw PipelineError{"audio_classification pipeline requires a .wav input file"};

        const PipelineManager::PipelineStage* stft_stage_ptr = nullptr;
        const PipelineManager::PipelineStage* tvm_stage_ptr  = nullptr;
        for (const auto& stage : state.pipeline_config.stages) {
            if (stage.message_type == "C7X_MSG_STFT_ANALYZE") stft_stage_ptr = &stage;
            else if (stage.message_type == "TVM_INFERENCE")   tvm_stage_ptr  = &stage;
        }
        if (!stft_stage_ptr)
            throw PipelineError{"audio_classification pipeline requires a C7X_MSG_STFT_ANALYZE stage"};
        if (!tvm_stage_ptr)
            throw PipelineError{"audio_classification pipeline requires a TVM_INFERENCE stage"};

        const auto& sp            = stft_stage_ptr->parameters;
        const size_t HOP_SIZE     = require_param(sp, "hop_size",     stft_stage_ptr->stage_id.c_str());
        const size_t MODEL_ELEMS  = require_param(sp, "model_elems",  stft_stage_ptr->stage_id.c_str());
        const size_t TOTAL_FRAMES = require_param(sp, "total_frames", stft_stage_ptr->stage_id.c_str());
        const size_t BATCH_N      = require_param(sp, "batch_n",      stft_stage_ptr->stage_id.c_str());
        const size_t NUM_BATCHES  = (TOTAL_FRAMES + BATCH_N - 1) / BATCH_N;

        const size_t audio_batch_bytes  = BATCH_N      * HOP_SIZE    * sizeof(int16_t);
        const size_t spectral_buf_bytes = TOTAL_FRAMES * MODEL_ELEMS * sizeof(float);

        std::cout << "[App] STFT parameters: HOP=" << HOP_SIZE
                  << " MODEL_ELEMS=" << MODEL_ELEMS
                  << " BATCH_N=" << BATCH_N
                  << " TOTAL_FRAMES=" << TOTAL_FRAMES << std::endl;

        // Parse TVM input shape from TVM stage parameters — required
        auto shape_it = tvm_stage_ptr->parameters.find("input_shape");
        if (shape_it == tvm_stage_ptr->parameters.end())
            throw PipelineError{"TVM stage missing required parameter: input_shape"};
        std::vector<int64_t> tvm_input_shape;
        {
            std::istringstream ss(shape_it->second);
            std::string token;
            while (std::getline(ss, token, ','))
                tvm_input_shape.push_back(std::stoll(token));
        }

        // Initialize TVM
        if (!state.tvm_artifacts_configured)
            throw PipelineError{"audio_classification pipeline requires TVM artifacts"};
        if (!tvm_client.is_initialized()) {
            if (!tvm_client.initialize(state.tvm_artifacts_paths[0]))
                throw PipelineError{"Failed to initialize TVM client"};
            std::cout << "[App] TVM initialized, input shape [";
            for (size_t i = 0; i < tvm_input_shape.size(); ++i)
                std::cout << tvm_input_shape[i] << (i + 1 < tvm_input_shape.size() ? "," : "");
            std::cout << "]" << std::endl;
        }

        // Load labels (optional)
        const auto class_labels = load_labels(state.pipeline_config.labels_path);

        auto label_for = [&](size_t idx) -> std::string {
            if (idx < class_labels.size()) return class_labels[idx];
            return "class_" + std::to_string(idx);
        };

        std::vector<int16_t> audio_data;
        if (!loadAudioFile(state.current_input_file, audio_data))
            throw PipelineError{"Failed to load input audio"};

        if (!dsp_client.initialize(state.pipeline_config.dsp_config.proc_id,
                                   state.pipeline_config.dsp_config.endpoint))
            throw PipelineError{"Failed to initialize DSP Task client"};

        DmaBuffer dma_audio{audio_batch_bytes,  "STFT audio input"};
        DmaBuffer dma_mel  {spectral_buf_bytes, "STFT mel output"};

        std::cout << "[App] DMA buffers:" << std::endl;
        std::cout << "[App]   audio in: phys=0x" << std::hex << dma_audio->phys_addr
                  << std::dec << " size=" << dma_audio->size << std::endl;
        std::cout << "[App]   mel out:  phys=0x" << std::hex << dma_mel->phys_addr
                  << std::dec << " size=" << dma_mel->size << std::endl;

        const size_t total_frames    = (audio_data.size() + HOP_SIZE - 1) / HOP_SIZE;
        audio_data.resize(total_frames * HOP_SIZE, int16_t{0});
        const size_t num_full_chunks = total_frames / TOTAL_FRAMES;

        std::cout << "[App] Total frames: " << total_frames
                  << " | Full chunks: " << num_full_chunks << std::endl;

        std::vector<float> chunk_input(TOTAL_FRAMES * MODEL_ELEMS);
        std::vector<float> chunk_scores;
        size_t tvm_chunks = 0;

        auto t_total = std::chrono::steady_clock::now();

        for (size_t chunk_idx = 0; chunk_idx < num_full_chunks; ++chunk_idx) {
            const size_t chunk_frame_offset = chunk_idx * TOTAL_FRAMES;
            auto t_chunk = std::chrono::steady_clock::now();

            // STFT batches
            for (size_t batch_idx = 0; batch_idx < NUM_BATCHES; ++batch_idx) {
                const size_t last_batch_frames  = TOTAL_FRAMES - BATCH_N * (NUM_BATCHES - 1);
                const size_t frames_this_batch  = (batch_idx < NUM_BATCHES - 1) ? BATCH_N : last_batch_frames;
                const size_t samples_this_batch = frames_this_batch * HOP_SIZE;
                const size_t audio_offset       = (chunk_frame_offset + batch_idx * BATCH_N) * HOP_SIZE;
                const uint64_t spectral_offset  = batch_idx * BATCH_N * MODEL_ELEMS * sizeof(float);

                dma_audio.begin_cpu_access();
                std::fill_n(dma_audio.data<std::byte>(), audio_batch_bytes, std::byte{});
                if (audio_offset < audio_data.size()) {
                    const size_t available = std::min(samples_this_batch,
                                                      audio_data.size() - audio_offset);
                    std::copy_n(audio_data.begin() + static_cast<std::ptrdiff_t>(audio_offset),
                                available, dma_audio.data<int16_t>());
                }
                dma_audio.end_cpu_access();

                auto params = stft_stage_ptr->parameters;
                params["input_buffer"]  = hex_address(dma_audio->phys_addr);
                params["output_buffer"] = hex_address(dma_mel->phys_addr + spectral_offset);
                params["input_frame"]   = std::to_string(frames_this_batch);
                params["output_frame"]  = std::to_string(frames_this_batch);

                if (debug)
                    std::cout << "[App]   batch " << (batch_idx + 1) << "/" << NUM_BATCHES
                              << ": " << frames_this_batch << " frames"
                              << " in=0x" << std::hex << dma_audio->phys_addr
                              << " out=0x" << (dma_mel->phys_addr + spectral_offset)
                              << std::dec << std::endl;

                auto r = dsp_client.process("C7X_MSG_STFT_ANALYZE", params);
                if (!r.success)
                    throw PipelineError{"STFT batch " + std::to_string(batch_idx + 1) +
                                        " failed: " + r.error_message};
            }

            // Copy mel from DMA and run TVM
            dma_mel.begin_cpu_access();
            std::copy_n(dma_mel.data<float>(), TOTAL_FRAMES * MODEL_ELEMS, chunk_input.begin());
            dma_mel.end_cpu_access();

            auto t_tvm = std::chrono::steady_clock::now();
            if (!tvm_client.run_inference(chunk_input, chunk_scores, tvm_input_shape))
                throw PipelineError{"TVM inference failed on chunk " +
                                    std::to_string(chunk_idx + 1)};
            double t_tvm_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t_tvm).count() / 1000.0;

            ++tvm_chunks;

            // num_classes: from config if set, otherwise use actual model output size
            const size_t num_classes = state.pipeline_config.num_classes > 0
                ? state.pipeline_config.num_classes
                : chunk_scores.size();

            const size_t top_n = std::min(size_t{5}, num_classes);
            std::vector<size_t> top(num_classes);
            std::iota(top.begin(), top.end(), 0);
            std::partial_sort(top.begin(), top.begin() + top_n, top.end(),
                [&](size_t a, size_t b) { return chunk_scores[a] > chunk_scores[b]; });

            double t_chunk_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t_chunk).count() / 1000.0;

            std::cout << "[App] Chunk " << (chunk_idx + 1) << "/" << num_full_chunks
                      << " STFT=" << std::fixed << std::setprecision(1)
                      << (t_chunk_ms - t_tvm_ms) << "ms"
                      << " TVM=" << t_tvm_ms << "ms"
                      << " total=" << t_chunk_ms << "ms" << std::endl;

            std::cout << "[App] Chunk " << (chunk_idx + 1) << " top-" << top_n << ":" << std::endl;
            for (size_t r = 0; r < top_n; ++r) {
                const size_t ci = top[r];
                std::cout << "[App]   " << (r + 1) << ". "
                          << std::fixed << std::setprecision(4) << chunk_scores[ci]
                          << "  " << label_for(ci) << std::endl;
            }
        }

        double t_total_ms = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t_total).count() / 1000.0;

        std::cout << "[App] Done: " << total_frames << " mel frames"
                  << " | " << tvm_chunks << " TVM chunks"
                  << " | " << std::fixed << std::setprecision(1) << t_total_ms << "ms" << std::endl;

        if (tvm_chunks == 0)
            std::cout << "[App] Warning: no full chunks processed (audio too short?)" << std::endl;

        std::cout << "PASSED" << std::endl;
        return PipelineManager::CommandResult::SUCCESS;

    } catch (const std::exception& error) {
        std::cerr << "[App] Pipeline failed: " << error.what() << std::endl;
        return PipelineManager::CommandResult::ERROR;
    }
}
