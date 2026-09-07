#include "stft_istft_pipeline.h"
#include "pipeline_common.h"
#include "audio_utils.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>


PipelineManager::CommandResult run_stft_istft_pipeline(
    PipelineManager::State& state,
    DspTaskClient& dsp_client,
    bool debug)
{
    try {
        // Find required stages
        const PipelineManager::PipelineStage* stft_stage_ptr  = nullptr;
        const PipelineManager::PipelineStage* deint_stage_ptr = nullptr;
        const PipelineManager::PipelineStage* inter_stage_ptr = nullptr;
        const PipelineManager::PipelineStage* istft_stage_ptr = nullptr;

        for (const auto& stage : state.pipeline_config.stages) {
            if (stage.message_type == "C7X_MSG_STFT_ANALYZE")          stft_stage_ptr  = &stage;
            else if (stage.message_type == "C7X_MSG_ISTFT_SYNTHESIZE") istft_stage_ptr = &stage;
            else if (stage.message_type == "C7X_DEINTERLEAVE_MSG_ANALYZE") {
                auto it = stage.parameters.find("flag");
                if (it != stage.parameters.end() && it->second == "0")
                    deint_stage_ptr = &stage;
                else
                    inter_stage_ptr = &stage;
            }
        }

        if (!stft_stage_ptr || !istft_stage_ptr || !deint_stage_ptr || !inter_stage_ptr)
            throw PipelineError{"stft_istft pipeline requires STFT, deinterleave, interleave and ISTFT stages"};

        // Read loop/buffer parameters from STFT stage
        const auto& sp = stft_stage_ptr->parameters;
        const size_t HOP_SIZE     = require_param(sp, "hop_size",     stft_stage_ptr->stage_id.c_str());
        const size_t MODEL_ELEMS  = require_param(sp, "model_elems",  stft_stage_ptr->stage_id.c_str());
        const size_t TOTAL_FRAMES = require_param(sp, "total_frames", stft_stage_ptr->stage_id.c_str());
        const size_t BATCH_N      = require_param(sp, "batch_n",      stft_stage_ptr->stage_id.c_str());
        const size_t PAD_FRAMES   = TOTAL_FRAMES % BATCH_N;
        const size_t NUM_BATCHES  = (TOTAL_FRAMES + BATCH_N - 1) / BATCH_N;

        // Read parameters from ISTFT stage
        const auto& ip = istft_stage_ptr->parameters;
        const size_t ISTFT_HOP_SIZE     = require_param(ip, "hop_size",     istft_stage_ptr->stage_id.c_str());
        const size_t ISTFT_MODEL_ELEMS  = require_param(ip, "model_elems",  istft_stage_ptr->stage_id.c_str());
        const size_t ISTFT_TOTAL_FRAMES = require_param(ip, "total_frames", istft_stage_ptr->stage_id.c_str());
        const size_t ISTFT_BATCH_N      = require_param(ip, "batch_n",      istft_stage_ptr->stage_id.c_str());

        // STFT-side buffer sizes (buf1, buf2, buf5)
        const size_t audio_batch_bytes    = BATCH_N      * HOP_SIZE    * sizeof(int16_t);
        const size_t spectral_stft_bytes  = TOTAL_FRAMES * MODEL_ELEMS * sizeof(float);

        // ISTFT-side buffer sizes (buf3, buf4, buf6)
        const size_t audio_istft_batch_bytes = ISTFT_BATCH_N      * ISTFT_HOP_SIZE    * sizeof(int16_t);
        const size_t spectral_istft_bytes    = ISTFT_TOTAL_FRAMES * ISTFT_MODEL_ELEMS * sizeof(float);

        std::cout << "[App] STFT parameters:  HOP=" << HOP_SIZE << " MODEL_ELEMS=" << MODEL_ELEMS
                  << " BATCH_N=" << BATCH_N << " TOTAL_FRAMES=" << TOTAL_FRAMES << std::endl;
        std::cout << "[App] ISTFT parameters: HOP=" << ISTFT_HOP_SIZE << " MODEL_ELEMS=" << ISTFT_MODEL_ELEMS
                  << " BATCH_N=" << ISTFT_BATCH_N << " TOTAL_FRAMES=" << ISTFT_TOTAL_FRAMES << std::endl;

        // Load audio
        std::vector<int16_t> audio_data;
        if (!loadAudioFile(state.current_input_file, audio_data))
            throw PipelineError{"Failed to load input audio"};
        const size_t original_sample_count = audio_data.size();

        if (!dsp_client.initialize(state.pipeline_config.dsp_config.proc_id,
                                   state.pipeline_config.dsp_config.endpoint))
            throw PipelineError{"Failed to initialize DSP Task client"};

        // Allocate DMA buffers
        DmaBuffer dma_buf1{audio_batch_bytes,       "STFT input"};
        DmaBuffer dma_buf2{spectral_stft_bytes,     "STFT output"};
        DmaBuffer dma_buf3{spectral_istft_bytes,    "interleave output"};
        DmaBuffer dma_buf4{audio_istft_batch_bytes, "ISTFT output"};
        DmaBuffer dma_buf5{spectral_stft_bytes,     "deinterleave output"};
        DmaBuffer dma_buf6{spectral_istft_bytes,    "interleave input"};

        std::cout << "[App] DMA buffers:" << std::endl;
        std::cout << "[App]   buf1 (STFT audio in):      phys=0x" << std::hex << dma_buf1->phys_addr
                  << std::dec << " size=" << dma_buf1->size << std::endl;
        std::cout << "[App]   buf2 (STFT out/deint in):  phys=0x" << std::hex << dma_buf2->phys_addr
                  << std::dec << " size=" << dma_buf2->size << std::endl;
        std::cout << "[App]   buf3 (int out/ISTFT in):   phys=0x" << std::hex << dma_buf3->phys_addr
                  << std::dec << " size=" << dma_buf3->size << std::endl;
        std::cout << "[App]   buf4 (ISTFT audio out):    phys=0x" << std::hex << dma_buf4->phys_addr
                  << std::dec << " size=" << dma_buf4->size << std::endl;
        std::cout << "[App]   buf5 (deint out):          phys=0x" << std::hex << dma_buf5->phys_addr
                  << std::dec << " size=" << dma_buf5->size << std::endl;
        std::cout << "[App]   buf6 (interleave in):      phys=0x" << std::hex << dma_buf6->phys_addr
                  << std::dec << " size=" << dma_buf6->size << std::endl;

        // Chunk calculation
        const size_t total_frames =
            (audio_data.size() + HOP_SIZE - 1) / HOP_SIZE;
        audio_data.resize(total_frames * HOP_SIZE, int16_t{0});
        const size_t num_full_chunks = total_frames / TOTAL_FRAMES;
        const size_t partial_frames  = total_frames % TOTAL_FRAMES;
        const size_t num_chunks      = num_full_chunks + (partial_frames > 0 ? 1 : 0);

        std::cout << "[App] Full file processing:" << std::endl;
        std::cout << "[App]   Total frames: " << total_frames
                  << " | Full chunks: " << num_full_chunks
                  << " | Partial chunk: " << partial_frames << " real frames"
                  << " (zero-padded to " << TOTAL_FRAMES << ")" << std::endl;

        std::vector<int16_t> processed_audio_data;
        processed_audio_data.reserve(total_frames * HOP_SIZE);

        AudioStream audio_stream;

        uint64_t stft_out_base  = dma_buf2->phys_addr;
        uint64_t istft_src_base = dma_buf3->phys_addr;

        std::vector<float> deint_output_data;
        std::vector<float> inter_input_data;

        auto t_total_start = std::chrono::steady_clock::now();

        for (size_t chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
            const size_t chunk_frame_offset = chunk_idx * TOTAL_FRAMES;
            const size_t num_batches_chunk  = NUM_BATCHES;
            const size_t real_frames_chunk  = (chunk_idx == num_full_chunks && partial_frames > 0)
                                              ? partial_frames : TOTAL_FRAMES;

            auto t_chunk_start = std::chrono::steady_clock::now();

            // Phase 1: STFT
            auto t_stft_start = std::chrono::steady_clock::now();
            for (size_t batch_idx = 0; batch_idx < num_batches_chunk; batch_idx++) {
                const size_t frames_this_batch  = (batch_idx < NUM_BATCHES - 1) ? BATCH_N : PAD_FRAMES;
                const size_t samples_this_batch = frames_this_batch * HOP_SIZE;
                const size_t audio_offset       = (chunk_frame_offset + batch_idx * BATCH_N) * HOP_SIZE;
                const size_t audio_bytes        = samples_this_batch * sizeof(int16_t);
                const uint64_t spectral_offset  = batch_idx * BATCH_N * MODEL_ELEMS * sizeof(float);

                dma_buf1.begin_cpu_access();
                std::fill_n(dma_buf1.data<std::byte>(), audio_batch_bytes, std::byte{});
                if (audio_offset < audio_data.size()) {
                    const size_t available = std::min(samples_this_batch,
                                                      audio_data.size() - audio_offset);
                    std::copy_n(audio_data.begin() + static_cast<std::ptrdiff_t>(audio_offset),
                                available, dma_buf1.data<int16_t>());
                }
                dma_buf1.end_cpu_access();
                audio_stream.send_frame(0, dma_buf1.data<std::byte>(), audio_bytes);

                auto params = stft_stage_ptr->parameters;
                params["input_buffer"]  = hex_address(dma_buf1->phys_addr);
                params["output_buffer"] = hex_address(stft_out_base + spectral_offset);
                params["input_frame"]   = std::to_string(frames_this_batch);
                params["output_frame"]  = std::to_string(frames_this_batch);

                if (debug)
                    std::cout << "[App]   STFT batch " << (batch_idx+1) << "/" << num_batches_chunk
                              << ": " << frames_this_batch << " frames"
                              << " in=0x" << std::hex << dma_buf1->phys_addr
                              << " out=0x" << (stft_out_base + spectral_offset) << std::dec << std::endl;

                auto r = dsp_client.process("C7X_MSG_STFT_ANALYZE", params);
                if (!r.success)
                    throw PipelineError{"STFT batch " + std::to_string(batch_idx + 1) +
                                        " failed: " + r.error_message};
            }
            double t_stft_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t_stft_start).count() / 1000.0;

            // Phase 2: Deinterleave
            if (debug)
                std::cout << "[App]   Deinterleave: 0x" << std::hex << dma_buf2->phys_addr
                          << " -> 0x" << dma_buf5->phys_addr << std::dec << std::endl;
            {
                auto params = deint_stage_ptr->parameters;
                params["input_buffer"]  = hex_address(dma_buf2->phys_addr);
                params["output_buffer"] = hex_address(dma_buf5->phys_addr);
                params["input_frame"]   = std::to_string(TOTAL_FRAMES);
                auto r = dsp_client.process("C7X_DEINTERLEAVE_MSG_ANALYZE", params);
                if (!r.success)
                    throw PipelineError{"Deinterleave failed: " + r.error_message};
            }

            // Phase 3: Pass-through (no TVM)
            deint_output_data.resize(spectral_stft_bytes / sizeof(float));
            inter_input_data.resize(spectral_istft_bytes / sizeof(float));

            dma_buf5.begin_cpu_access();
            std::copy_n(dma_buf5.data<float>(), deint_output_data.size(),
                        deint_output_data.begin());
            dma_buf5.end_cpu_access();

            inter_input_data = deint_output_data;

            // Phase 4: Interleave
            dma_buf6.begin_cpu_access();
            std::copy(inter_input_data.begin(), inter_input_data.end(),
                      dma_buf6.data<float>());
            dma_buf6.end_cpu_access();

            if (debug)
                std::cout << "[App]   Interleave: 0x" << std::hex << dma_buf6->phys_addr
                          << " -> 0x" << dma_buf3->phys_addr << std::dec << std::endl;
            {
                auto params = inter_stage_ptr->parameters;
                params["input_buffer"]  = hex_address(dma_buf6->phys_addr);
                params["output_buffer"] = hex_address(dma_buf3->phys_addr);
                params["input_frame"]   = std::to_string(TOTAL_FRAMES);
                auto r = dsp_client.process("C7X_DEINTERLEAVE_MSG_ANALYZE", params);
                if (!r.success)
                    throw PipelineError{"Interleave failed: " + r.error_message};
            }

            // Phase 5: ISTFT
            const size_t ISTFT_PAD_FRAMES  = ISTFT_TOTAL_FRAMES % ISTFT_BATCH_N;
            const size_t ISTFT_NUM_BATCHES = (ISTFT_TOTAL_FRAMES + ISTFT_BATCH_N - 1) / ISTFT_BATCH_N;
            auto t_istft_start = std::chrono::steady_clock::now();
            size_t real_samples_remaining = real_frames_chunk * ISTFT_HOP_SIZE;
            for (size_t batch_idx = 0; batch_idx < ISTFT_NUM_BATCHES; batch_idx++) {
                const size_t frames_this_batch  = (batch_idx < ISTFT_NUM_BATCHES - 1) ? ISTFT_BATCH_N : ISTFT_PAD_FRAMES;
                const size_t samples_this_batch = frames_this_batch * ISTFT_HOP_SIZE;
                const size_t audio_offset       = (chunk_frame_offset + batch_idx * ISTFT_BATCH_N) * ISTFT_HOP_SIZE;
                const uint64_t spectral_offset  = batch_idx * ISTFT_BATCH_N * ISTFT_MODEL_ELEMS * sizeof(float);

                auto params = istft_stage_ptr->parameters;
                params["input_buffer"]  = hex_address(istft_src_base + spectral_offset);
                params["output_buffer"] = hex_address(dma_buf4->phys_addr);
                params["input_frame"]   = std::to_string(frames_this_batch);
                params["output_frame"]  = std::to_string(frames_this_batch);

                if (debug)
                    std::cout << "[App]   ISTFT batch " << (batch_idx+1) << "/" << num_batches_chunk
                              << ": " << frames_this_batch << " frames"
                              << " in=0x" << std::hex << (istft_src_base + spectral_offset)
                              << " out=0x" << dma_buf4->phys_addr << std::dec << std::endl;

                auto r = dsp_client.process("C7X_MSG_ISTFT_SYNTHESIZE", params);
                if (!r.success)
                    throw PipelineError{"ISTFT batch " + std::to_string(batch_idx + 1) +
                                        " failed: " + r.error_message};

                dma_buf4.begin_cpu_access();
                const auto* out_ptr = dma_buf4.data<int16_t>();

                if (debug) {
                    std::cout << "[App]   Frame | InRMS  OutRMS | In[0..4]              | Out[0..4]" << std::endl;
                    const size_t available_frames = std::min(
                        frames_this_batch, real_samples_remaining / ISTFT_HOP_SIZE);
                    for (size_t f = 0; f < std::min(size_t{4}, available_frames); ++f) {
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
                                  << std::sqrt(in_sum / HOP_SIZE) << "  "
                                  << std::sqrt(out_sum / HOP_SIZE)
                                  << " | In:";
                        for (size_t i = 0; i < 5; i++)
                            std::cout << std::setw(6) << audio_data[in_off + i] << (i<4?",":"");
                        std::cout << " | Out:";
                        for (size_t i = 0; i < 5; i++)
                            std::cout << std::setw(6) << out_ptr[out_off + i] << (i<4?",":"");
                        std::cout << std::endl;
                    }
                }

                size_t samples_to_collect = std::min(samples_this_batch, real_samples_remaining);
                std::copy_n(out_ptr, samples_to_collect,
                            std::back_inserter(processed_audio_data));
                real_samples_remaining -= samples_to_collect;

                dma_buf4.end_cpu_access();
                audio_stream.send_frame(1, out_ptr, samples_to_collect * sizeof(int16_t));
            }
            double t_istft_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t_istft_start).count() / 1000.0;

            double t_chunk_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t_chunk_start).count() / 1000.0;

            std::cout << "[App] Chunk " << (chunk_idx+1) << "/" << num_chunks
                      << " [" << real_frames_chunk << " real frames"
                      << (real_frames_chunk < ISTFT_TOTAL_FRAMES ? " + zero-pad" : "") << "]"
                      << " | STFT=" << std::fixed << std::setprecision(1) << t_stft_ms << "ms"
                      << " TVM=0.0ms"
                      << " ISTFT=" << t_istft_ms << "ms"
                      << " total=" << t_chunk_ms << "ms" << std::endl;
        }

        double t_total_ms = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t_total_start).count() / 1000.0;

        std::cout << "[App] All chunks done | total=" << std::fixed << std::setprecision(1)
                  << t_total_ms << "ms | " << (processed_audio_data.size() / ISTFT_HOP_SIZE)
                  << " output frames (" << processed_audio_data.size() << " samples)" << std::endl;

        if (processed_audio_data.size() < original_sample_count)
            throw PipelineError{"Pipeline produced fewer samples than expected"};
        processed_audio_data.resize(original_sample_count);

        std::string output_filename = "processed_output.wav";
        if (!saveAudioFile(output_filename, processed_audio_data))
            throw PipelineError{"Failed to save output file"};
        std::cout << "[App] Saved to " << output_filename << std::endl;
        return PipelineManager::CommandResult::SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "[App] Pipeline failed: " << error.what() << std::endl;
        return PipelineManager::CommandResult::ERROR;
    }
}
