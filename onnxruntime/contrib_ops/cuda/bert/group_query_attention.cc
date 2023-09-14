// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/cuda/cuda_common.h"
#include "core/platform/env_var_utils.h"
#include "contrib_ops/cuda/bert/group_query_attention_impl.h"
#include "contrib_ops/cuda/bert/group_query_attention.h"
#include "contrib_ops/cuda/bert/group_query_attention_helper.h"
#include "contrib_ops/cuda/bert/flash_attention/flash_api.h"
// #include "contrib_ops/cuda/transformers/dump_cuda_tensor.h"
// #include "contrib_ops/cpu/utils/console_dumper.h"

using namespace onnxruntime::cuda;
using namespace ::onnxruntime::common;
using namespace ONNX_NAMESPACE;

namespace onnxruntime {
namespace contrib {
namespace cuda {

#define REGISTER_KERNEL_TYPED(T)                                  \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                  \
      GroupQueryAttention,                                         \
      kMSDomain,                                                  \
      1,                                                          \
      T,                                                          \
      kCudaExecutionProvider,                                     \
      (*KernelDefBuilder::Create()) \
          .TypeConstraint("T", DataTypeImpl::GetTensorType<T>()) \
          .TypeConstraint("M", DataTypeImpl::GetTensorType<int32_t>()) \
          .InputMemoryType(OrtMemTypeCPUInput, 5), \
      GroupQueryAttention<T>);

// REGISTER_KERNEL_TYPED(float) // TODO(aciddelgado): support regular float?
REGISTER_KERNEL_TYPED(MLFloat16)

template <typename T>
GroupQueryAttention<T>::GroupQueryAttention(const OpKernelInfo& info)
    : CudaKernel(info) {
  int64_t num_heads = 0;
  int64_t kv_num_heads = 0;
  ORT_ENFORCE(info.GetAttr("num_heads", &num_heads).IsOK() && num_heads > 0);
  ORT_ENFORCE(info.GetAttr("kv_num_heads", &kv_num_heads).IsOK() && kv_num_heads > 0 && num_heads % kv_num_heads == 0);
  num_heads_ = static_cast<int>(num_heads);
  kv_num_heads_ = static_cast<int>(kv_num_heads);
  is_unidirectional_ = info.GetAttrOrDefault<int64_t>("unidirectional", 1) == 1;
  scale_ = info.GetAttrOrDefault<float>("scale", 0.0f);

#if USE_FLASH_ATTENTION
  disable_flash_attention_ = sizeof(T) != 2 ||
                             ParseEnvironmentVariableWithDefault<bool>(attention::kDisableFlashAttention, false);
#else
  disable_flash_attention_ = true;
#endif
}

template <typename T>
Status GroupQueryAttention<T>::ComputeInternal(OpKernelContext* context) const {
  const Tensor* query = context->Input<Tensor>(0);
  const Tensor* key = context->Input<Tensor>(1);
  const Tensor* value = context->Input<Tensor>(2);
  const Tensor* past_key = context->Input<Tensor>(3);
  const Tensor* past_value = context->Input<Tensor>(4);
  const Tensor* past_seq_len = context->Input<Tensor>(5);

  auto& device_prop = GetDeviceProp();
  GroupQueryAttentionParameters parameters;
  // TODO(aciddelgado): we need to cross check past_sequence_length_ as well
  ORT_RETURN_IF_ERROR(group_query_attention_helper::CheckInputs<Tensor>(query,
                                                                        key,
                                                                        value,
                                                                        past_key,
                                                                        past_value,
                                                                        &parameters,
                                                                        num_heads_,
                                                                        kv_num_heads_,
                                                                        past_seq_len,
                                                                        scale_,
                                                                        device_prop.maxThreadsPerBlock));
  parameters.is_unidirectional = is_unidirectional_;
  int sequence_length = parameters.sequence_length;

  TensorShapeVector output_shape(3);
  output_shape[0] = static_cast<int64_t>(parameters.batch_size);
  output_shape[1] = static_cast<int64_t>(sequence_length);
  output_shape[2] = static_cast<int64_t>(parameters.hidden_size);
  Tensor* output = context->Output(0, output_shape);

  std::vector<int64_t> present_dims{
      parameters.batch_size, parameters.kv_num_heads, parameters.max_sequence_length, parameters.head_size};
  TensorShape present_shape(present_dims);
  // TODO(aciddelgado): we need to inplace present and past, as in they need to point to same place
  Tensor* present_key = context->Output(1, present_shape);
  Tensor* present_value = context->Output(2, present_shape);
#if USE_FLASH_ATTENTION
  bool use_flash_attention = !disable_flash_attention_ &&
                             onnxruntime::flash::is_supported(device_prop,
                                                              parameters.head_size,
                                                              parameters.num_heads,
                                                              parameters.kv_num_heads);
#else
  constexpr bool use_flash_attention = false;
#endif

  // constexpr size_t element_size = sizeof(T);
  // size_t workspace_bytes = GetAttentionWorkspaceSize(element_size,
  //                                                   parameters.batch_size,
  //                                                   parameters.num_heads,
  //                                                   parameters.kv_num_heads,
  //                                                   parameters.head_size,
  //                                                   parameters.sequence_length,
  //                                                   parameters.kv_sequence_length,
  //                                                   parameters.total_sequence_length,
  //                                                   use_flash_attention);
  // auto work_space = GetScratchBuffer<void>(workspace_bytes, context->GetComputeStream());

  // TODO(aciddelgado): what are these temp work space and i should probably remove
  // const size_t past_kv_bytes = element_size * parameters.batch_size * parameters.past_sequence_length * parameters.kv_num_heads * parameters.head_size;
  // const bool use_temp_k_v_workspace = use_flash_attention;
  // auto temp_k_work_space = use_temp_k_v_workspace ? GetScratchBuffer<void>(past_kv_bytes, context->GetComputeStream()) : nullptr;
  // auto temp_v_work_space = use_temp_k_v_workspace ? GetScratchBuffer<void>(past_kv_bytes, context->GetComputeStream()) : nullptr;

  typedef typename ToCudaType<T>::MappedType CudaT;
  GroupQueryAttentionData<CudaT> data;

  size_t softmax_lse_bytes = onnxruntime::flash::get_softmax_lse_size(parameters.sequence_length, parameters.batch_size, parameters.num_heads);
  auto softmax_lse_buffer = GetScratchBuffer<void>(softmax_lse_bytes, context->GetComputeStream());
  data.softmax_lse = reinterpret_cast<CudaT*>(softmax_lse_buffer.get());

  // split kv buffers
  // TODO(aciddelgado): should we restructure the inputs of the node to reflect the flash api, eg past_key vs key vs key_cache?
  // TODO(aciddelgado): restructure these functions so they take parameters as input rather than individual numbers
  parameters.num_splits = onnxruntime::flash::num_splits_heuristic(
      parameters.batch_size, parameters.sequence_length, parameters.max_sequence_length, parameters.num_heads,
      parameters.head_size, device_prop.multiProcessorCount, 128, past_key != nullptr);
  if (parameters.num_splits > 1) {
    // softmax_lse_accum buffer
    size_t softmax_lse_accum_bytes = onnxruntime::flash::get_softmax_lse_accum_size(
        parameters.num_splits, parameters.batch_size, parameters.num_heads, parameters.sequence_length);
    auto softmax_lse_accum_buffer = GetScratchBuffer<void>(softmax_lse_accum_bytes, context->GetComputeStream());
    data.softmax_lse_accum = reinterpret_cast<CudaT*>(softmax_lse_accum_buffer.get());
    // out_accum buffer
    auto round_multiple = [](int x, int m) { return (x + m - 1) / m * m; };
    const int head_size_rounded = round_multiple(parameters.head_size, 32);
    size_t out_accum_bytes = onnxruntime::flash::get_out_accum_size(
        parameters.num_splits, parameters.batch_size, parameters.num_heads, parameters.sequence_length, head_size_rounded);
    auto out_accum_buffer = GetScratchBuffer<void>(out_accum_bytes, context->GetComputeStream());
    data.out_accum = reinterpret_cast<CudaT*>(out_accum_buffer.get());
  } else {
    // unnecessary if no split kv
    data.softmax_lse_accum = nullptr;
    data.out_accum = nullptr;
  }

  // seqlens_k buffer
  if (past_key != nullptr) {
    size_t seqlens_k_bytes = sizeof(int) * parameters.batch_size;
    auto seqlens_k_buffer = GetScratchBuffer<void>(seqlens_k_bytes, context->GetComputeStream());
    data.seqlens_k = reinterpret_cast<int*>(seqlens_k_buffer.get());
  }

  data.query = reinterpret_cast<const CudaT*>(query->Data<T>());
  data.key = reinterpret_cast<const CudaT*>(key->Data<T>());
  data.value = reinterpret_cast<const CudaT*>(value->Data<T>());
  data.past = nullptr;
  data.past_key = (nullptr == past_key) ? nullptr : reinterpret_cast<const CudaT*>(past_key->Data<T>());
  data.past_value = (nullptr == past_value) ? nullptr : reinterpret_cast<const CudaT*>(past_value->Data<T>());
  data.has_qkv_workspace = true;
  // data.workspace = reinterpret_cast<CudaT*>(work_space.get());
  // data.temp_k_workspace = use_temp_k_v_workspace ? reinterpret_cast<CudaT*>(temp_k_work_space.get()) : nullptr;
  // data.temp_v_workspace = use_temp_k_v_workspace ? reinterpret_cast<CudaT*>(temp_v_work_space.get()) : nullptr;
  data.output = reinterpret_cast<CudaT*>(output->MutableData<T>());
  data.present = nullptr;
  // TODO(aciddelgado): present should point to past...
  data.present_key = (nullptr == present_key) ? nullptr : reinterpret_cast<CudaT*>(present_key->MutableData<T>());
  data.present_value = (nullptr == present_value) ? nullptr : reinterpret_cast<CudaT*>(present_value->MutableData<T>());
  data.use_flash_attention = use_flash_attention;

  cublasHandle_t cublas = GetCublasHandle(context);

  return QkvToContext<CudaT>(
      device_prop, cublas, context->GetComputeStream(), parameters, data);
}

}  // namespace cuda
}  // namespace contrib
}  // namespace onnxruntime
