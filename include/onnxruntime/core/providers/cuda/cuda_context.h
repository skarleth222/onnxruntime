// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "cuda_resource.h"
#include <core/session/onnxruntime_cxx_api.h>
#include <cuda_runtime.h>
#include <cublas_v2.h>
#include <cudnn.h>

#define ORT_CUDA_CTX

namespace Ort {

namespace Custom {

struct CudaContext {
  void* raw_stream = {};

  cudaStream_t cuda_stream = {};
  cudnnHandle_t cudnn_handle = {};
  cublasHandle_t cublas_handle = {};

  void Init(const OrtKernelContext& kernel_ctx) {
    void* stream = {};
    const auto& ort_api = Ort::GetApi();
    void* resource = {};
    OrtStatus* status = nullptr;

    status = ort_api.KernelContext_GetResource(&kernel_ctx, ORT_CUDA_RESOUCE_VERSION, CudaResource::cuda_stream_t, &resource);
    if (status) {
      ORT_CXX_API_THROW("failed to fetch cuda stream", OrtErrorCode::ORT_RUNTIME_EXCEPTION);
    }
    cuda_stream = reinterpret_cast<cudaStream_t>(resource);

    resource = {};
    status = ort_api.KernelContext_GetResource(&kernel_ctx, ORT_CUDA_RESOUCE_VERSION, CudaResource::cudnn_handle_t, &resource);
    if (status) {
		ORT_CXX_API_THROW("failed to fetch cudnn handle", OrtErrorCode::ORT_RUNTIME_EXCEPTION);
    }
    cudnn_handle = reinterpret_cast<cudnnHandle_t>(resource);

    resource = {};
    status = ort_api.KernelContext_GetResource(&kernel_ctx, ORT_CUDA_RESOUCE_VERSION, CudaResource::cublas_handle_t, &resource);
    if (status) {
      ORT_CXX_API_THROW("failed to fetch cublas handle", OrtErrorCode::ORT_RUNTIME_EXCEPTION);
    }
    cublas_handle = reinterpret_cast<cublasHandle_t>(resource);
  }
};

}  // namespace Custom
}  // namespace Ort