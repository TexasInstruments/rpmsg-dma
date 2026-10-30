#include "speech_enhancement_pipeline.h"
#include "pipeline_common.h"
#include "audio_utils.h"
#include "speech_chunk_range.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>


PipelineManager::CommandResult run_speech_enhancement_pipeline(
    PipelineManager::State& state,
    DspTaskClient& dsp_client,
    TvmInferenceClient& tvm_client,
    bool debug)
{
    try {
        // Find required stages
        const PipelineManager::PipelineStage* stft_stage_ptr   = nullptr;
        const PipelineManager::PipelineStage* deint_stage_ptr  = nullptr;
        const PipelineManager::PipelineStage* tvm_stage_ptr    = nullptr;
        const PipelineManager::PipelineStage* inter_stage_ptr  = nullptr;
        const PipelineManager::PipelineStage* istft_stage_ptr  = nullptr;

        for (const auto& stage : state.pipeline_config.stages) {
            if (stage.message_type == "C7X_MSG_STFT_ANALYZE")          stft_stage_ptr  = &stage;
            else if (stage.message_type == "C7X_MSG_ISTFT_SYNTHESIZE") istft_stage_ptr = &stage;
            else if (stage.message_type == "TVM_INFERENCE")            tvm_stage_ptr   = &stage;
            else if (stage.message_type == "C7X_DEINTERLEAVE_MSG_ANALYZE") {
                auto it = stage.parameters.find("flag");
                if (it != stage.parameters.end() && it->second == "0")
                    deint_stage_ptr = &stage;
                else
                    inter_stage_ptr = &stage;
            }
        }

        if (!stft_stage_ptr || !istft_stage_ptr || !deint_stage_ptr || !inter_stage_ptr)
            throw PipelineError{"speech_enhancement pipeline requires STFT, deinterleave, interleave and ISTFT stages"};

        // Read loop/buffer parameters from STFT stage (drives STFT-side buffers and loop)
        const auto& sp = stft_stage_ptr->parameters;
        const size_t HOP_SIZE       = require_param(sp, "hop_size",     stft_stage_ptr->stage_id.c_str());
        const size_t MODEL_ELEMS    = require_param(sp, "model_elems",  stft_stage_ptr->stage_id.c_str());
        const size_t TOTAL_FRAMES   = require_param(sp, "total_frames", stft_stage_ptr->stage_id.c_str());
        const size_t BATCH_N        = require_param(sp, "batch_n",      stft_stage_ptr->stage_id.c_str());
        const size_t NUM_BATCHES    = (TOTAL_FRAMES + BATCH_N - 1) / BATCH_N;

        // Read parameters from ISTFT stage (drives ISTFT-side buffers — TVM output may differ)
        const auto& ip = istft_stage_ptr->parameters;
        const size_t ISTFT_HOP_SIZE     = require_param(ip, "hop_size",     istft_stage_ptr->stage_id.c_str());
        const size_t ISTFT_MODEL_ELEMS  = require_param(ip, "model_elems",  istft_stage_ptr->stage_id.c_str());
        const size_t ISTFT_TOTAL_FRAMES = require_param(ip, "total_frames", istft_stage_ptr->stage_id.c_str());
        const size_t ISTFT_BATCH_N      = require_param(ip, "batch_n",      istft_stage_ptr->stage_id.c_str());

        if (state.pipeline_config.overlap_frames < -1 ||
            (state.pipeline_config.overlap_frames > 0 &&
             static_cast<size_t>(state.pipeline_config.overlap_frames) >= TOTAL_FRAMES))
            throw PipelineError{"overlap_frames must be -1 (disabled) or in [0, total_frames)"};
        if (ISTFT_HOP_SIZE != HOP_SIZE || ISTFT_TOTAL_FRAMES != TOTAL_FRAMES)
            throw PipelineError{"Speech reconstruction requires matching STFT/ISTFT hop_size and total_frames"};

        // STFT-side payload sizes
        const size_t audio_batch_bytes       = BATCH_N      * HOP_SIZE    * sizeof(int16_t);
        const size_t spectral_stft_bytes     = TOTAL_FRAMES * MODEL_ELEMS * sizeof(float);

        // ISTFT-side payload sizes — based on TVM output shape
        const size_t audio_istft_batch_bytes = ISTFT_BATCH_N      * ISTFT_HOP_SIZE    * sizeof(int16_t);
        const size_t spectral_istft_bytes    = ISTFT_TOTAL_FRAMES * ISTFT_MODEL_ELEMS * sizeof(float);

        std::cout << "[App] STFT parameters:  HOP=" << HOP_SIZE << " MODEL_ELEMS=" << MODEL_ELEMS
                  << " BATCH_N=" << BATCH_N << " TOTAL_FRAMES=" << TOTAL_FRAMES << std::endl;
        std::cout << "[App] ISTFT parameters: HOP=" << ISTFT_HOP_SIZE << " MODEL_ELEMS=" << ISTFT_MODEL_ELEMS
                  << " BATCH_N=" << ISTFT_BATCH_N << " TOTAL_FRAMES=" << ISTFT_TOTAL_FRAMES << std::endl;
        std::cout << "[App]   STFT spectral buffer: " << spectral_stft_bytes << " bytes" << std::endl;
        std::cout << "[App]   ISTFT spectral buffer: " << spectral_istft_bytes << " bytes" << std::endl;

        // Load audio
        std::vector<int16_t> audio_data;
        if (!loadAudioFile(state.current_input_file, audio_data))
            throw PipelineError{"Failed to load input audio"};
        const size_t original_sample_count = audio_data.size();

        if (!dsp_client.initialize(state.pipeline_config.dsp_config.proc_id,
                                   state.pipeline_config.dsp_config.endpoint))
            throw PipelineError{"Failed to initialize DSP Task client"};

        // Two reusable DMA allocations; every DSP process() waits for completion.
        // A: PCM batch in -> planar STFT -> planar model output -> PCM batch out.
        // B: interleaved STFT -> interleaved model output.
        // De/interleave always use distinct allocations. During STFT, all spectra
        // accumulate in B while A is refilled; during ISTFT, B stays intact while
        // each PCM batch in A is copied into chunk_output before A is reused.
        // The synchronous TVM call consumes A before replacing its contents.
        // It retains no DMA views after returning.
        const size_t work_a_bytes = std::max({audio_batch_bytes,
                                              audio_istft_batch_bytes,
                                              spectral_stft_bytes,
                                              spectral_istft_bytes});
        const size_t work_b_bytes = std::max(spectral_stft_bytes, spectral_istft_bytes);
        DmaBuffer dma_work_a{work_a_bytes, "speech PCM / planar spectra"};
        DmaBuffer dma_work_b{work_b_bytes, "speech interleaved spectra"};

        std::cout << "[App] DMA buffers (reused across sequential stages):" << std::endl;
        std::cout << "[App]   A (PCM / planar):  phys=0x" << std::hex << dma_work_a->phys_addr
                  << std::dec << " size=" << dma_work_a->size << std::endl;
        std::cout << "[App]   B (interleaved):   phys=0x" << std::hex << dma_work_b->phys_addr
                  << std::dec << " size=" << dma_work_b->size << std::endl;

        // Chunking parameters — overlap-save only if overlap_frames set in JSON
        const bool   USE_OVERLAP     = state.pipeline_config.overlap_frames > 0;
        const size_t OVERLAP_FRAMES  = USE_OVERLAP
                                       ? static_cast<size_t>(state.pipeline_config.overlap_frames)
                                       : 0;
        const size_t HOP_FRAMES      = USE_OVERLAP ? TOTAL_FRAMES - OVERLAP_FRAMES : TOTAL_FRAMES;
        const size_t HOP_SAMPLES     = HOP_FRAMES * HOP_SIZE;
        const size_t CHUNK_SAMPLES   = TOTAL_FRAMES * HOP_SIZE;

        const size_t n_samples = audio_data.size();
        size_t n_chunks;
        if (n_samples <= CHUNK_SAMPLES) {
            n_chunks = 1;
        } else {
            n_chunks = 1 + static_cast<size_t>(
                std::ceil(static_cast<double>(n_samples - CHUNK_SAMPLES) / HOP_SAMPLES));
        }
        const size_t padded_len = (n_chunks - 1) * HOP_SAMPLES + CHUNK_SAMPLES;
        audio_data.resize(padded_len, int16_t{0});

        if (USE_OVERLAP) {
            std::cout << "[App] Overlap-save chunking:" << std::endl;
            std::cout << "[App]   TOTAL_FRAMES=" << TOTAL_FRAMES
                      << " OVERLAP_FRAMES=" << OVERLAP_FRAMES
                      << " HOP_FRAMES=" << HOP_FRAMES
                      << " n_chunks=" << n_chunks << std::endl;
        } else {
            std::cout << "[App] Sequential chunking (no overlap):" << std::endl;
            std::cout << "[App]   TOTAL_FRAMES=" << TOTAL_FRAMES
                      << " n_chunks=" << n_chunks << std::endl;
        }

        // Parse TVM input shape from stage parameters — required, e.g. "1,2,401,161"
        std::vector<int64_t> tvm_input_shape;
        if (tvm_stage_ptr) {
            auto shape_it = tvm_stage_ptr->parameters.find("input_shape");
            if (shape_it == tvm_stage_ptr->parameters.end())
                throw PipelineError{"TVM stage missing required parameter: input_shape"};
            std::istringstream ss(shape_it->second);
            std::string token;
            while (std::getline(ss, token, ','))
                tvm_input_shape.push_back(std::stoll(token));
        }

        // Initialize TVM
        if (tvm_stage_ptr && state.tvm_artifacts_configured && !tvm_client.is_initialized()) {
            if (!tvm_client.initialize(state.tvm_artifacts_paths[0]))
                throw PipelineError{"Failed to initialize TVM client"};
            std::cout << "[App] TVM initialized, input shape [";
            for (size_t i = 0; i < tvm_input_shape.size(); ++i)
                std::cout << tvm_input_shape[i] << (i + 1 < tvm_input_shape.size() ? "," : "");
            std::cout << "]" << std::endl;
        }

        std::vector<int16_t> processed_audio_data;
        processed_audio_data.reserve(n_samples);

        uint64_t stft_out_base  = dma_work_b->phys_addr;
        uint64_t istft_src_base = dma_work_b->phys_addr;

        AudioStream audio_stream;

        auto t_total_start = std::chrono::steady_clock::now();
        bool first_stft_batch  = true;
        bool first_istft_batch = true;

        for (size_t chunk_idx = 0; chunk_idx < n_chunks; chunk_idx++) {
            // Sample offset for this chunk (overlap-save: chunks are HOP_SAMPLES apart)
            const size_t chunk_sample_offset  = chunk_idx * HOP_SAMPLES;
            const size_t chunk_frame_offset   = chunk_sample_offset / HOP_SIZE;
            const size_t num_batches_chunk    = NUM_BATCHES;

            const auto keep = speech_chunk_range(chunk_idx, n_chunks, CHUNK_SAMPLES,
                                                 OVERLAP_FRAMES, HOP_SIZE,
                                                 original_sample_count);
            const size_t lo_sample = keep.begin;
            const size_t hi_sample = keep.end;

            auto t_chunk_start = std::chrono::steady_clock::now();

            // Phase 1: STFT
            auto t_stft_start = std::chrono::steady_clock::now();
            for (size_t batch_idx = 0; batch_idx < num_batches_chunk; batch_idx++) {
                const size_t frames_this_batch  = std::min(BATCH_N, TOTAL_FRAMES - batch_idx * BATCH_N);
                const size_t samples_this_batch = frames_this_batch * HOP_SIZE;
                const size_t audio_offset       = chunk_sample_offset + batch_idx * BATCH_N * HOP_SIZE;
                const uint64_t spectral_offset  = batch_idx * BATCH_N * MODEL_ELEMS * sizeof(float);

                dma_work_a.begin_cpu_access();
                std::fill_n(dma_work_a.data<std::byte>(), audio_batch_bytes, std::byte{});
                if (audio_offset < audio_data.size()) {
                    const size_t available = std::min(samples_this_batch,
                                                      audio_data.size() - audio_offset);
                    std::copy_n(audio_data.begin() + static_cast<std::ptrdiff_t>(audio_offset),
                                available, dma_work_a.data<int16_t>());
                }
                dma_work_a.end_cpu_access();

                auto params = stft_stage_ptr->parameters;
                params["input_buffer"]  = hex_address(dma_work_a->phys_addr);
                params["output_buffer"] = hex_address(stft_out_base + spectral_offset);
                params["input_frame"]   = std::to_string(frames_this_batch);
                params["output_frame"]  = std::to_string(frames_this_batch);
                params["clear"]         = first_stft_batch ? "1" : "0";
                first_stft_batch = false;

                if (debug)
                    std::cout << "[App]   STFT batch " << (batch_idx+1) << "/" << num_batches_chunk
                              << ": " << frames_this_batch << " frames"
                              << " in=0x" << std::hex << dma_work_a->phys_addr
                              << " out=0x" << (stft_out_base + spectral_offset)
                              << std::dec << " clear=" << params["clear"] << std::endl;

                auto r = dsp_client.process("C7X_MSG_STFT_ANALYZE", params);
                if (!r.success)
                    throw PipelineError{"STFT batch " + std::to_string(batch_idx + 1) +
                                        " failed: " + r.error_message};
            }
            double t_stft_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t_stft_start).count() / 1000.0;

            // Phase 2: Deinterleave
            if (debug)
                std::cout << "[App]   Deinterleave: 0x" << std::hex << dma_work_b->phys_addr
                          << " -> 0x" << dma_work_a->phys_addr << std::dec << std::endl;
            {
                auto params = deint_stage_ptr->parameters;
                params["input_buffer"]  = hex_address(dma_work_b->phys_addr);
                params["output_buffer"] = hex_address(dma_work_a->phys_addr);
                params["input_frame"]   = std::to_string(TOTAL_FRAMES);
                auto r = dsp_client.process("C7X_DEINTERLEAVE_MSG_ANALYZE", params);
                if (!r.success)
                    throw PipelineError{"Deinterleave failed: " + r.error_message};
            }

            // Phase 3: TVM
            double t_tvm_ms = 0.0;
            auto t_tvm_start = std::chrono::steady_clock::now();

            if (tvm_client.is_initialized()) {
                // CPU reads the planar DSP output and writes the model result
                // directly through A's mapping. Keep CPU ownership across the
                // synchronous call, including socket I/O when using the daemon.
                dma_work_a.begin_cpu_access();
                bool inference_ok;
                try {
                    inference_ok = tvm_client.run_inference(
                        dma_work_a.data<float>(), spectral_stft_bytes / sizeof(float),
                        dma_work_a.data<float>(), spectral_istft_bytes / sizeof(float),
                        tvm_input_shape);
                } catch (...) {
                    dma_work_a.end_cpu_access();
                    throw;
                }
                dma_work_a.end_cpu_access();
                if (!inference_ok)
                    throw PipelineError{"TVM inference failed"};
                t_tvm_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t_tvm_start).count() / 1000.0;
            } else if (spectral_stft_bytes != spectral_istft_bytes) {
                throw PipelineError{"Bypass output size does not match ISTFT spectral buffer"};
            }
            // With inference disabled, A already holds the planar bypass data.

            // Phase 4: Interleave
            if (debug)
                std::cout << "[App]   Interleave: 0x" << std::hex << dma_work_a->phys_addr
                          << " -> 0x" << dma_work_b->phys_addr << std::dec << std::endl;
            {
                auto params = inter_stage_ptr->parameters;
                params["input_buffer"]  = hex_address(dma_work_a->phys_addr);
                params["output_buffer"] = hex_address(dma_work_b->phys_addr);
                params["input_frame"]   = std::to_string(TOTAL_FRAMES);
                auto r = dsp_client.process("C7X_DEINTERLEAVE_MSG_ANALYZE", params);
                if (!r.success)
                    throw PipelineError{"Interleave failed: " + r.error_message};
            }

            // Phase 5: ISTFT — collect full chunk output into temporary buffer
            const size_t ISTFT_NUM_BATCHES = (ISTFT_TOTAL_FRAMES + ISTFT_BATCH_N - 1) / ISTFT_BATCH_N;
            std::vector<int16_t> chunk_output;
            chunk_output.reserve(CHUNK_SAMPLES);

            auto t_istft_start = std::chrono::steady_clock::now();
            for (size_t batch_idx = 0; batch_idx < ISTFT_NUM_BATCHES; batch_idx++) {
                const size_t frames_this_batch  = std::min(ISTFT_BATCH_N, ISTFT_TOTAL_FRAMES - batch_idx * ISTFT_BATCH_N);
                const size_t samples_this_batch = frames_this_batch * ISTFT_HOP_SIZE;
                const size_t audio_offset       = chunk_sample_offset + batch_idx * ISTFT_BATCH_N * ISTFT_HOP_SIZE;
                const uint64_t spectral_offset  = batch_idx * ISTFT_BATCH_N * ISTFT_MODEL_ELEMS * sizeof(float);

                auto params = istft_stage_ptr->parameters;
                params["input_buffer"]  = hex_address(istft_src_base + spectral_offset);
                params["output_buffer"] = hex_address(dma_work_a->phys_addr);
                params["input_frame"]   = std::to_string(frames_this_batch);
                params["output_frame"]  = std::to_string(frames_this_batch);
                params["clear"]         = first_istft_batch ? "1" : "0";
                first_istft_batch = false;

                if (debug)
                    std::cout << "[App]   ISTFT batch " << (batch_idx+1) << "/" << ISTFT_NUM_BATCHES
                              << ": " << frames_this_batch << " frames"
                              << " in=0x" << std::hex << (istft_src_base + spectral_offset)
                              << " out=0x" << dma_work_a->phys_addr
                              << std::dec << " clear=" << params["clear"] << std::endl;

                auto r = dsp_client.process("C7X_MSG_ISTFT_SYNTHESIZE", params);
                if (!r.success)
                    throw PipelineError{"ISTFT batch " + std::to_string(batch_idx + 1) +
                                        " failed: " + r.error_message};

                dma_work_a.begin_cpu_access();
                const auto* out_ptr = dma_work_a.data<int16_t>();

                if (debug) {
                    std::cout << "[App]   Frame | InRMS  OutRMS | In[0..4]              | Out[0..4]" << std::endl;
                    for (size_t f = 0; f < std::min(size_t{4}, frames_this_batch); ++f) {
                        const size_t in_off  = audio_offset + f * ISTFT_HOP_SIZE;
                        const size_t out_off = f * ISTFT_HOP_SIZE;
                        float in_sum = 0.0f, out_sum = 0.0f;
                        for (size_t i = 0; i < ISTFT_HOP_SIZE; i++) {
                            const float s = static_cast<float>(audio_data[in_off + i]) / 32768.0f;
                            const float o = static_cast<float>(out_ptr[out_off + i]) / 32768.0f;
                            in_sum += s * s;
                            out_sum += o * o;
                        }
                        std::cout << "[App]   " << std::setw(5)
                                  << (chunk_frame_offset + batch_idx * ISTFT_BATCH_N + f + 1)
                                  << " | " << std::fixed << std::setprecision(4)
                                  << std::sqrt(in_sum / ISTFT_HOP_SIZE) << "  "
                                  << std::sqrt(out_sum / ISTFT_HOP_SIZE)
                                  << " | In:";
                        for (size_t i = 0; i < 5; i++)
                            std::cout << std::setw(6) << audio_data[in_off + i] << (i<4?",":"");
                        std::cout << " | Out:";
                        for (size_t i = 0; i < 5; i++)
                            std::cout << std::setw(6) << out_ptr[out_off + i] << (i<4?",":"");
                        std::cout << std::endl;
                    }
                }

                std::copy_n(out_ptr, samples_this_batch, std::back_inserter(chunk_output));
                dma_work_a.end_cpu_access();
            }
            double t_istft_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t_istft_start).count() / 1000.0;

            // Trim-reconstruct: keep [lo_sample..hi_sample) from this chunk
            const size_t keep_end = std::min(hi_sample, chunk_output.size());
            if (lo_sample < keep_end) {
                std::copy(chunk_output.begin() + static_cast<std::ptrdiff_t>(lo_sample),
                          chunk_output.begin() + static_cast<std::ptrdiff_t>(keep_end),
                          std::back_inserter(processed_audio_data));
                // Publish only finalized samples, identical to the saved WAV. Pair
                // input/output on the same source timeline and bound message size.
                for (size_t offset = lo_sample; offset < keep_end; offset += 1024) {
                    const size_t count = std::min(size_t{1024}, keep_end - offset);
                    audio_stream.send_frame(0, audio_data.data() + chunk_sample_offset + offset,
                                            count * sizeof(int16_t));
                    audio_stream.send_frame(1, chunk_output.data() + offset,
                                            count * sizeof(int16_t));
                }
            }

            double t_chunk_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t_chunk_start).count() / 1000.0;

            std::cout << "[App] Chunk " << (chunk_idx+1) << "/" << n_chunks
                      << " [keep samples " << lo_sample << ".." << keep_end << "]"
                      << " | STFT=" << std::fixed << std::setprecision(1) << t_stft_ms << "ms"
                      << " TVM=" << t_tvm_ms << "ms"
                      << " ISTFT=" << t_istft_ms << "ms"
                      << " total=" << t_chunk_ms << "ms" << std::endl;
        }

        double t_total_ms = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t_total_start).count() / 1000.0;

        std::cout << "[App] All chunks done | total=" << std::fixed << std::setprecision(1)
                  << t_total_ms << "ms | " << (processed_audio_data.size() / ISTFT_HOP_SIZE)
                  << " output frames (" << processed_audio_data.size() << " samples)" << std::endl;

        // Trim to original length (remove any zero-padding)
        if (processed_audio_data.size() > original_sample_count)
            processed_audio_data.resize(original_sample_count);

        std::string output_filename = "processed_output.wav";
        if (!saveAudioFile(output_filename, processed_audio_data))
            throw PipelineError{"Failed to save output file"};
        std::cout << "[App] Saved to " << output_filename << std::endl;
	std::cout << "PASSED" << std::endl;
        return PipelineManager::CommandResult::SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "[App] Pipeline failed: " << error.what() << std::endl;
        return PipelineManager::CommandResult::ERROR;
    }
}
