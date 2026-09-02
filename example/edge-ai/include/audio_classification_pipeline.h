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

#endif // AUDIO_CLASSIFICATION_PIPELINE_H
