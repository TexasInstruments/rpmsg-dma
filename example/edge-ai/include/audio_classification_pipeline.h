#ifndef AUDIO_CLASSIFICATION_PIPELINE_H
#define AUDIO_CLASSIFICATION_PIPELINE_H

#include "pipeline_manager.h"
#include "dsp_task_client.h"
#include "tvm_inference_client.h"

PipelineManager::CommandResult run_audio_classification_pipeline(
    PipelineManager::State& state,
    DspTaskClient& dsp_client,
    TvmInferenceClient& tvm_client,
    bool debug);

/* Streaming variant: reads raw S16LE PCM from stdin indefinitely instead of a WAV file.
 * Runs STFT+TVM on each TOTAL_FRAMES*HOP_SIZE sample window and flushes top-N results
 * to stdout immediately.  Exits cleanly on EOF (stdin closed). */
PipelineManager::CommandResult run_audio_classification_pipeline_stream(
    PipelineManager::State& state,
    DspTaskClient& dsp_client,
    TvmInferenceClient& tvm_client,
    bool debug);

/* ALSA capture variant: opens alsa_device directly (S16LE, 16 kHz, mono) and
 * runs inference windows indefinitely until the process receives SIGTERM/SIGINT.
 * No external arecord process required — the binary owns the audio device. */
PipelineManager::CommandResult run_audio_classification_pipeline_alsa(
    PipelineManager::State& state,
    DspTaskClient& dsp_client,
    TvmInferenceClient& tvm_client,
    bool debug,
    const std::string& alsa_device);

#endif // AUDIO_CLASSIFICATION_PIPELINE_H
