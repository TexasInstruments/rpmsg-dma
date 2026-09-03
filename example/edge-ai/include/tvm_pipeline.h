#ifndef TVM_PIPELINE_H
#define TVM_PIPELINE_H

#include "pipeline_manager.h"
#include "tvm_inference_client.h"

PipelineManager::CommandResult run_tvm_pipeline(
    PipelineManager::State& state,
    TvmInferenceClient& tvm_client);

#endif // TVM_PIPELINE_H
