/* Copyright 2016 Carnegie Mellon University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "scanner/eval/evaluator.h"
#include "scanner/eval/evaluator_factory.h"
#include "scanner/evaluators/caffe/net_descriptor.h"
#include "scanner/util/opencv.h"

namespace scanner {

class YoloInputEvaluator : public Evaluator {
 public:
  YoloInputEvaluator(DeviceType device_type, i32 device_id,
                     const NetDescriptor& descriptor, i32 batch_size);

  void configure(const BatchConfig& config) override;

  void evaluate(const BatchedColumns& input_columns,
                BatchedColumns& output_columns) override;

 private:
  DeviceType device_type_;
  i32 device_id_;
  NetDescriptor descriptor_;
  InputFormat metadata_;
  i32 batch_size_;

  static const i32 NET_INPUT_WIDTH = 448;
  static const i32 NET_INPUT_HEIGHT = 448;

  cv::Mat mean_mat;

  cv::Mat resized_input;
  cv::Mat bgr_input;
  std::vector<cv::Mat> input_planes;
  cv::Mat planar_input;
  cv::Mat float_input;
  cv::Mat normalized_input;
};

class YoloInputEvaluatorFactory : public EvaluatorFactory {
 public:
  YoloInputEvaluatorFactory(DeviceType device_type,
                            const NetDescriptor& descriptor, i32 batch_size);

  EvaluatorCapabilities get_capabilities() override;

  std::vector<std::string> get_output_columns(
      const std::vector<std::string>& input_columns) override;

  Evaluator* new_evaluator(const EvaluatorConfig& config) override;

 private:
  DeviceType device_type_;
  NetDescriptor net_descriptor_;
  i32 batch_size_;
};
}