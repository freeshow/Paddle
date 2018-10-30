/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "paddle/fluid/framework/feed_fetch_method.h"
#include "paddle/fluid/inference/api/api_impl.h"
#include "paddle/fluid/inference/api/details/reset_tensor_array.h"
#include "paddle/fluid/inference/api/helper.h"
#include "paddle/fluid/inference/api/timer.h"
#include "paddle/fluid/platform/cpu_helper.h"
#include "paddle/fluid/platform/profiler.h"

DEFINE_bool(profile, false, "Turn on profiler for fluid");
DECLARE_int32(paddle_num_threads);

namespace paddle {

void NativePaddlePredictor::PrepareFeedFetch() {
  for (auto *op : inference_program_->Block(0).AllOps()) {
    if (op->Type() == "feed") {
      int idx = boost::get<int>(op->GetAttr("col"));
      if (feeds_.size() <= static_cast<size_t>(idx)) {
        feeds_.resize(idx + 1);
      }
      feeds_[idx] = op;
      feed_names_[op->Output("Out")[0]] = idx;
    } else if (op->Type() == "fetch") {
      int idx = boost::get<int>(op->GetAttr("col"));
      if (fetchs_.size() <= static_cast<size_t>(idx)) {
        fetchs_.resize(idx + 1);
      }
      fetchs_[idx] = op;
    }
  }
}

bool NativePaddlePredictor::Init(
    std::shared_ptr<framework::Scope> parent_scope) {
#if !defined(_WIN32)
  if (FLAGS_profile) {
    LOG(WARNING) << "Profiler is actived, might affect the performance";
    LOG(INFO) << "You can turn off by set gflags '-profile false'";

    auto tracking_device = config_.use_gpu ? platform::ProfilerState::kAll
                                           : platform::ProfilerState::kCPU;
    platform::EnableProfiler(tracking_device);
  }
#endif

  // no matter with or without MKLDNN
  paddle::platform::SetNumThreads(FLAGS_paddle_num_threads);

  if (config_.use_gpu) {
    place_ = paddle::platform::CUDAPlace(config_.device);
  } else {
    place_ = paddle::platform::CPUPlace();
  }
  if (parent_scope) {
    scope_ = parent_scope;
    sub_scope_ = &(parent_scope->NewScope());
    PADDLE_ENFORCE_NOT_NULL(sub_scope_, "create sub scope fail");
  } else {
    paddle::framework::InitDevices(false);
    scope_.reset(new paddle::framework::Scope());
  }
  executor_.reset(new paddle::framework::Executor(place_));
  // Initialize the inference program
  if (!config_.model_dir.empty()) {
    // Parameters are saved in separate files sited in
    // the specified `dirname`.
    inference_program_ = paddle::inference::Load(executor_.get(), scope_.get(),
                                                 config_.model_dir);

  } else if (!config_.prog_file.empty() && !config_.param_file.empty()) {
    // All parameters are saved in a single file.
    // The file names should be consistent with that used
    // in Python API `fluid.io.save_inference_model`.
    auto exe = executor_.get();
    auto sc = scope_.get();
    inference_program_ = paddle::inference::Load(
        executor_.get(), scope_.get(), config_.prog_file, config_.param_file);

  } else {
    LOG(ERROR) << "fail to load inference model from " << config_.model_dir;
    return false;
  }

  ctx_ = executor_->Prepare(*inference_program_, 0);
  executor_->CreateVariables(*inference_program_,
                             sub_scope_ ? sub_scope_ : scope_.get(), 0);

  // Get the feed_target_names and fetch_target_names
  PrepareFeedFetch();
  return true;
}

NativePaddlePredictor::~NativePaddlePredictor() {
#if !defined(_WIN32)
  if (FLAGS_profile) {
    platform::DisableProfiler(platform::EventSortingKey::kTotal,
                              "./profile.log");
  }
#endif
  if (sub_scope_) {
    scope_->DeleteScope(sub_scope_);
  }
}

bool NativePaddlePredictor::Run(const std::vector<PaddleTensor> &inputs,
                                std::vector<PaddleTensor> *output_data,
                                int batch_size) {
  using Timer = paddle::inference::Timer;
  Timer timer;
  timer.tic();
  // set feed variable
  std::vector<framework::LoDTensor> feeds;
  framework::Scope *scope = sub_scope_ != nullptr ? sub_scope_ : scope_.get();
  if (!SetFeed(inputs, scope)) {
    LOG(ERROR) << "fail to set feed";
    return false;
  }
  // Run the inference program
  // if share variables, we need not create variables
  executor_->RunPreparedContext(ctx_.get(), scope,
                                false, /* don't create local scope each time*/
                                false /* don't create variable each time */);
  // get fetch variable
  if (!GetFetch(output_data, scope)) {
    LOG(ERROR) << "fail to get fetches";
    return false;
  }
  VLOG(3) << "predict cost: " << timer.toc() << "ms";

  // Fix TensorArray reuse not cleaned bug.
  tensor_array_batch_cleaner_.CollectTensorArrays(scope_.get());
  tensor_array_batch_cleaner_.ResetTensorArray();
  return true;
}

std::unique_ptr<PaddlePredictor> NativePaddlePredictor::Clone() {
  std::unique_ptr<PaddlePredictor> cls(new NativePaddlePredictor(config_));

  if (!dynamic_cast<NativePaddlePredictor *>(cls.get())->Init(scope_)) {
    LOG(ERROR) << "fail to call Init";
    return nullptr;
  }
#ifdef __clang__
  // fix clang compile error
  return cls;
#else
  // fix manylinux compile error.
  return std::move(cls);
#endif
}

bool NativePaddlePredictor::SetFeed(const std::vector<PaddleTensor> &inputs,
                                    framework::Scope *scope) {
  if (inputs.size() != feeds_.size()) {
    LOG(ERROR) << "wrong feed input size, need " << feeds_.size() << " but get "
               << inputs.size();
    return false;
  }
  for (size_t i = 0; i < inputs.size(); ++i) {
    framework::LoDTensor input;
    framework::DDim ddim = framework::make_ddim(inputs[i].shape);
    void *input_ptr;
    if (inputs[i].dtype == PaddleDType::INT64) {
      input_ptr = input.mutable_data<int64_t>(ddim, platform::CPUPlace());
    } else if (inputs[i].dtype == PaddleDType::FLOAT32) {
      input_ptr = input.mutable_data<float>(ddim, platform::CPUPlace());
    } else {
      LOG(ERROR) << "unsupported feed type " << inputs[i].dtype;
      return false;
    }

    // TODO(panyx0718): Init LoDTensor from existing memcpy to save a copy.
    std::memcpy(static_cast<void *>(input_ptr), inputs[i].data.data(),
                inputs[i].data.length());
    // TODO(Superjomn) Low performance, need optimization for heavy LoD copy.
    framework::LoD lod;
    for (auto &level : inputs[i].lod) {
      lod.emplace_back(level);
    }
    input.set_lod(lod);
    int idx = -1;
    if (config_.specify_input_name) {
      idx = feed_names_[inputs[i].name];
    } else {
      idx = boost::get<int>(feeds_[i]->GetAttr("col"));
    }
    framework::SetFeedVariable(scope, input, "feed", idx);
  }
  return true;
}
template <typename T>
void NativePaddlePredictor::GetFetchOne(const framework::LoDTensor &fetch,
                                        PaddleTensor *output) {
  // set shape.
  auto shape = framework::vectorize(fetch.dims());
  output->shape.assign(shape.begin(), shape.end());
  // set data.
  const T *data = fetch.data<T>();
  int num_elems = inference::VecReduceToInt(shape);
  output->data.Resize(num_elems * sizeof(T));
  // The fetched tensor output by fetch op, should always in CPU memory, so just
  // copy.
  memcpy(output->data.data(), data, num_elems * sizeof(T));
  // set lod
  output->lod.clear();
  for (auto &level : fetch.lod()) {
    output->lod.emplace_back(level.begin(), level.end());
  }
}

bool NativePaddlePredictor::GetFetch(std::vector<PaddleTensor> *outputs,
                                     framework::Scope *scope) {
  outputs->resize(fetchs_.size());
  for (size_t i = 0; i < fetchs_.size(); ++i) {
    int idx = boost::get<int>(fetchs_[i]->GetAttr("col"));
    PADDLE_ENFORCE((size_t)idx == i);
    framework::LoDTensor &fetch =
        framework::GetFetchVariable(*scope, "fetch", idx);
    auto type = fetch.type();
    auto output = &(outputs->at(i));
    if (type == typeid(float)) {
      GetFetchOne<float>(fetch, output);
      output->dtype = PaddleDType::FLOAT32;
    } else if (type == typeid(int64_t)) {
      GetFetchOne<int64_t>(fetch, output);
      output->dtype = PaddleDType::INT64;
    } else {
      LOG(ERROR) << "unknown type, only support float32 and int64 now.";
    }
  }
  return true;
}

template <>
std::unique_ptr<PaddlePredictor> CreatePaddlePredictor<
    NativeConfig, PaddleEngineKind::kNative>(const NativeConfig &config) {
  if (config.use_gpu) {
    // 1. GPU memeroy
    PADDLE_ENFORCE_GT(
       config.fraction_of_gpu_memory, 0.f,
       "fraction_of_gpu_memory in the config should be set to range (0.,
       1.]");
    PADDLE_ENFORCE_GE(config.device, 0, "Invalid device id %d", config.device);
    std::vector<std::string> flags;
    if (config.fraction_of_gpu_memory >= 0.0f ||
        config.fraction_of_gpu_memory <= 0.95f) {
      flags.push_back("dummpy");
      std::string flag = "--fraction_of_gpu_memory_to_use=" +
                         std::to_string(config.fraction_of_gpu_memory);
      flags.push_back(flag);
      framework::InitGflags(flags);
    }
  }
  std::unique_ptr<PaddlePredictor> predictor(new NativePaddlePredictor(config));
  if (!dynamic_cast<NativePaddlePredictor *>(predictor.get())->Init(nullptr)) {
    return nullptr;
  }
#ifdef __clang__
  // fix clang compile error
  return predictor;
#else
  return std::move(predictor);
#endif
}

template <>
std::unique_ptr<PaddlePredictor> CreatePaddlePredictor<NativeConfig>(
    const NativeConfig &config) {
  return CreatePaddlePredictor<NativeConfig, PaddleEngineKind::kNative>(config);
}

}  // namespace paddle
