#ifndef STFT_ISTFT_PIPELINE_H
#define STFT_ISTFT_PIPELINE_H

#include "pipeline_manager.h"
#include "dsp_task_client.h"

PipelineManager::CommandResult run_stft_istft_pipeline(
    PipelineManager::State& state,
    DspTaskClient& dsp_client,
    bool debug);

#endif // STFT_ISTFT_PIPELINE_H
