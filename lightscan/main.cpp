/* Copyright 2016 Carnegie Mellon University, NVIDIA Corporation
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

#include "lightscan/storage/storage_config.h"
#include "lightscan/storage/storage_backend.h"
#include "lightscan/util/common.h"
#include "lightscan/util/video.h"
#include "lightscan/util/caffe.h"
#include "lightscan/util/queue.h"
#include "lightscan/util/jpeg/JPEGWriter.h"

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/errors.hpp>

#include <opencv2/opencv.hpp>
#include <opencv2/core/cuda.hpp>
#include <opencv2/cudawarping.hpp>
#include <opencv2/cudaarithm.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/core/cuda_stream_accessor.hpp>
#include "lightscan/util/opencv.h"

#ifdef HARDWARE_DECODE
#include <cuda.h>
#include "lightscan/util/cuda.h"
#endif

#include <thread>
#include <mpi.h>
#include <pthread.h>
#include <cstdlib>
#include <string>
#include <libgen.h>
#include <atomic>

extern "C" {
#include "libavformat/avformat.h"
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

using namespace lightscan;
namespace po = boost::program_options;

///////////////////////////////////////////////////////////////////////////////
/// Global constants
int GPUS_PER_NODE = 1;           // Number of available GPUs per node
int GLOBAL_BATCH_SIZE = 64;      // Batch size for network
int BATCHES_PER_WORK_ITEM = 4;   // How many batches per work item
int TASKS_IN_QUEUE_PER_GPU = 4;  // How many tasks per GPU to allocate to a node
int LOAD_WORKERS_PER_NODE = 2;   // Number of worker threads loading data
int NUM_CUDA_STREAMS = 32;       // Number of cuda streams for image processing

const std::string DB_PATH = "/Users/abpoms/kcam";
const std::string IFRAME_PATH_POSTFIX = "_iframes";
const std::string METADATA_PATH_POSTFIX = "_metadata";
const std::string PROCESSED_VIDEO_POSTFIX = "_processed";

///////////////////////////////////////////////////////////////////////////////
/// Helper functions

std::string processed_video_path(const std::string& video_path) {
  return dirname_s(video_path) + "/" +
    basename_s(video_path) + PROCESSED_VIDEO_POSTFIX + ".mp4";
}

std::string metadata_path(const std::string& video_path) {
  return dirname_s(video_path) + "/" +
    basename_s(video_path) + METADATA_PATH_POSTFIX + ".bin";
}

std::string iframe_path(const std::string& video_path) {
  return dirname_s(video_path) + "/" +
    basename_s(video_path) + IFRAME_PATH_POSTFIX + ".bin";
}

inline int frames_per_work_item() {
  return GLOBAL_BATCH_SIZE * BATCHES_PER_WORK_ITEM;
}

///////////////////////////////////////////////////////////////////////////////
/// Work structs
struct VideoWorkItem {
  int video_index;
  int start_frame;
  int end_frame;
};

struct LoadWorkEntry {
  int work_item_index;
};

struct LoadBufferEntry {
  int gpu_device_id;
  int buffer_index;
};

struct EvalWorkEntry {
  int work_item_index;
  int buffer_index;
};

///////////////////////////////////////////////////////////////////////////////
/// Worker thread arguments
struct LoadThreadArgs {
  // Uniform arguments
  const std::vector<std::string>& video_paths;
  const std::vector<VideoMetadata>& metadata;
  const std::vector<VideoWorkItem>& work_items;

  // Per worker arguments
  StorageConfig* storage_config;
#ifdef HARDWARE_DECODE
  std::vector<CUcontext> cuda_contexts; // context to use to decode frames
#endif

  // Queues for communicating work
  Queue<LoadWorkEntry>& load_work;
  Queue<LoadBufferEntry>& empty_load_buffers;
  std::vector<Queue<EvalWorkEntry>>& eval_work;

  // Buffers for loading frames into
  size_t buffer_size;
  char*** gpu_frame_buffers;
};

struct EvaluateThreadArgs {
  // Uniform arguments
  const std::vector<VideoMetadata>& metadata;
  const std::vector<VideoWorkItem>& work_items;

  // Per worker arguments
  int gpu_device_id; // for hardware decode, need to know gpu

  // Queues for communicating work
  Queue<EvalWorkEntry>& eval_work;
  Queue<LoadBufferEntry>& empty_load_buffers;

  // Buffers for reading frames from
  size_t buffer_size;
  char** frame_buffers;
};

///////////////////////////////////////////////////////////////////////////////
/// Thread to asynchronously load video
void convert_av_frame_to_rgb(
  SwsContext*& sws_context,
  AVFrame* frame,
  char* buffer)
{
  size_t buffer_size =
    av_image_get_buffer_size(AV_PIX_FMT_RGB24, frame->width, frame->height, 1);

  // Convert image to RGB
  sws_context = sws_getCachedContext(
    sws_context,

    frame->width, frame->height,
    static_cast<AVPixelFormat>(frame->format),

    frame->width, frame->height, AV_PIX_FMT_RGB24,
    SWS_BICUBIC, 0, 0, 0);

  if (sws_context == nullptr) {
    fprintf(stderr, "Error trying to get sws context\n");
    assert(false);
  }

  AVFrame rgb_format;
  int alloc_fail = av_image_alloc(rgb_format.data,
                                  rgb_format.linesize,
                                  frame->width,
                                  frame->height,
                                  AV_PIX_FMT_RGB24,
                                  1);

  if (alloc_fail < 0) {
    fprintf(stderr, "Error while allocating avpicture for conversion\n");
    assert(false);
  }

  sws_scale(sws_context,
            frame->data /* input data */,
            frame->linesize /* input layout */,
            0 /* x start location */,
            frame->height /* height of input image */,
            rgb_format.data /* output data */,
            rgb_format.linesize /* output layout */);

  av_image_copy_to_buffer(reinterpret_cast<uint8_t*>(buffer),
                          buffer_size,
                          rgb_format.data,
                          rgb_format.linesize,
                          AV_PIX_FMT_RGB24,
                          frame->width,
                          frame->height,
                          1);

  av_freep(&rgb_format.data[0]);
}

void* load_video_thread(void* arg) {
  LoadThreadArgs& args = *reinterpret_cast<LoadThreadArgs*>(arg);

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  // Setup a distinct storage backend for each IO thread
  StorageBackend* storage =
    StorageBackend::make_from_config(args.storage_config);

  std::vector<double> task_times;
  std::vector<double> idle_times;

  std::vector<double> io_times;
  std::vector<double> decode_times;
  std::vector<double> video_times;
  std::vector<double> memcpy_times;
  while (true) {
    auto idle_start1 = now();

    LoadWorkEntry load_work_entry;
    args.load_work.pop(load_work_entry);

    if (load_work_entry.work_item_index == -1) {
      break;
    }

    double idle_time = nano_since(idle_start1);

    auto start1 = now();

    const VideoWorkItem& work_item =
      args.work_items[load_work_entry.work_item_index];

    const std::string& video_path = args.video_paths[work_item.video_index];
    const VideoMetadata& metadata = args.metadata[work_item.video_index];

    // Open the iframe file to setup keyframe data
    std::string iframe_file_path = iframe_path(video_path);
    std::vector<int> keyframe_positions;
    std::vector<int64_t> keyframe_timestamps;
    {
      RandomReadFile* iframe_file;
      storage->make_random_read_file(iframe_file_path, iframe_file);

      (void)read_keyframe_info(
        iframe_file, 0, keyframe_positions, keyframe_timestamps);

      delete iframe_file;
    }

    // Open the video file for reading
    RandomReadFile* file;
    storage->make_random_read_file(video_path, file);

    double task_time = nano_since(start1);

    auto idle_start2 = now();

    LoadBufferEntry buffer_entry;
    args.empty_load_buffers.pop(buffer_entry);

    idle_times.push_back(idle_time + nano_since(idle_start2));

    auto start2 = now();

    CU_CHECK(cudaSetDevice(buffer_entry.gpu_device_id));

    char** frame_buffers = args.gpu_frame_buffers[buffer_entry.gpu_device_id];
    char* frame_buffer = frame_buffers[buffer_entry.buffer_index];

#ifdef HARDWARE_DECODE
    VideoDecoder decoder(args.cuda_contexts[buffer_entry.gpu_device_id],
                         file, keyframe_positions, keyframe_timestamps);
#else
    VideoDecoder decoder(file, keyframe_positions, keyframe_timestamps);
#endif

    decoder.seek(work_item.start_frame);

    size_t frame_size =
      av_image_get_buffer_size(AV_PIX_FMT_NV12,
                               metadata.width,
                               metadata.height,
                               1);


    double video_time = 0;
    double memcpy_time = 0;

    SwsContext* sws_context;
    int current_frame = work_item.start_frame;
    while (current_frame < work_item.end_frame) {
      auto video_start = now();

      AVFrame* frame = decoder.decode();
      assert(frame != nullptr);

      video_time += nano_since(video_start);

      size_t frames_buffer_offset =
        frame_size * (current_frame - work_item.start_frame);
      assert(frames_buffer_offset < args.buffer_size);
      char* current_frame_buffer_pos =
        frame_buffer + frames_buffer_offset;

#ifdef HARDWARE_DECODE
      // HACK(apoms): NVIDIA GPU decoder only outputs NV12 format so we rely
      //              on that here to copy the data properly
      auto memcpy_start = now();
      for (int i = 0; i < 2; i++) {
        CU_CHECK(cudaMemcpy2D(
          current_frame_buffer_pos + i * metadata.width * metadata.height,
          metadata.width, // dst pitch
          frame->data[i], // src
          frame->linesize[i], // src pitch
          frame->width, // width
          frame->height, // height
          cudaMemcpyDeviceToDevice));
      }
      memcpy_time += nano_since(memcpy_start);
#else
      convert_av_frame_to_rgb(sws_context, frame, current_frame_buffer_pos);
#endif
      current_frame++;
    }

    video_times.push_back(decode_time);
    io_times.push_back(decoder.time_spent_on_io());
    decode_times.push_back(decoder.time_spent_on_decode());
    memcpy_times.push_back(memcpy_time);

    task_times.push_back(task_time + nano_since(start2));

    EvalWorkEntry eval_work_entry;
    eval_work_entry.work_item_index = load_work_entry.work_item_index;
    eval_work_entry.buffer_index = buffer_entry.buffer_index;

    args.eval_work[buffer_entry.gpu_device_id].push(eval_work_entry);

    delete file;
  }

  double total_task_time = 0;
  for (double t : task_times) {
    total_task_time += t;
  }
  total_task_time /= 1000000; // convert from ns to ms
  double mean_task_time = total_task_time / task_times.size();
  double std_dev_task_time = 0;
  for (double t : task_times) {
    std_dev_task_time += std::pow(t / 1000000 - mean_task_time, 2);
  }
  std_dev_task_time = std::sqrt(std_dev_task_time / task_times.size());

  double total_idle_time = 0;
  for (double t : idle_times) {
    total_idle_time += t;
  }
  total_idle_time /= 1000000; // convert from ns to ms

  double total_memcpy_time = 0;
  for (double t : memcpy_times) {
    total_memcpy_time += t;
  }
  total_mempcy_time /= 1000000;

  double total_video_time = 0;
  for (double t : video_times) {
    total_video_time += t;
  }
  total_video_time /= 1000000;

  double total_decode_time = 0;
  for (double t : decode_times) {
    total_decode_time += t;
  }
  total_decode_time /= 1000000;

  double total_io_time = 0;
  for (double t : io_times) {
    total_io_time += t;
  }
  total_io_time /= 1000000;

  printf("(N: %d) Load thread finished. "
         "Total: %.3fms,  # Tasks: %lu, Mean: %.3fms, Std: %.3fms, "
         "Idle: %.3fms %3.2f\%\n"
         "Memcpy: %3.2f\%, Video: %3.2f\%, IO: %3.2f\%, Decode: %3.2f\%\n",
         rank,
         total_task_time, task_times.size(), mean_task_time, std_dev_task_time,
         total_idle_time,
         total_idle_time / (total_idle_time + total_task_time) * 100,
         total_mempcy_time / (total_task_time) * 100,
         total_video_time / (total_task_time) * 100,
         total_io_time / (total_task_time) * 100,
         total_decode_time / (total_task_time) * 100);

  // Cleanup
  delete storage;

  THREAD_RETURN_SUCCESS();
}

///////////////////////////////////////////////////////////////////////////////
/// Thread to run net evaluation
void* evaluate_thread(void* arg) {
  EvaluateThreadArgs& args = *reinterpret_cast<EvaluateThreadArgs*>(arg);

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  CU_CHECK(cudaSetDevice(args.gpu_device_id));
  // Setup caffe net
  NetInfo net_info = load_neural_net(NetType::ALEX_NET, args.gpu_device_id);
  caffe::Net<float>* net = net_info.net;

  int dim = net_info.input_size;

  cv::cuda::setDevice(args.gpu_device_id);

  cv::Mat cpu_mean_mat(
    net_info.mean_width, net_info.mean_height, CV_32FC3, net_info.mean_image);
  cv::cuda::GpuMat unsized_mean_mat(cpu_mean_mat);
  cv::cuda::GpuMat mean_mat;
  cv::cuda::resize(unsized_mean_mat, mean_mat, cv::Size(dim, dim));


  caffe::Blob<float> net_input{GLOBAL_BATCH_SIZE, 3, dim, dim};

  // OpenCV matrices
  std::vector<cv::cuda::Stream> cv_streams(NUM_CUDA_STREAMS);

#ifndef HARDWARE_DECODE
  std::vector<cv::cuda::GpuMat> input_mats(
    NUM_CUDA_STREAMS,
    cv::cuda::GpuMat(args.metadata[0].height + args.metadata[0].height / 2,
                     args.metadata[0].width,
                     CV_8UC1));
#endif

  std::vector<cv::cuda::GpuMat> rgba_mat(
    NUM_CUDA_STREAMS,
    cv::cuda::GpuMat(args.metadata[0].height, args.metadata[0].width, CV_8UC4));

  std::vector<cv::cuda::GpuMat> rgb_mat(
    NUM_CUDA_STREAMS,
    cv::cuda::GpuMat(args.metadata[0].height, args.metadata[0].width, CV_8UC3));

  std::vector<cv::cuda::GpuMat> conv_input(
    NUM_CUDA_STREAMS,
    cv::cuda::GpuMat(dim, dim, CV_8UC4));

  std::vector<cv::cuda::GpuMat> float_conv_input(
    NUM_CUDA_STREAMS,
    cv::cuda::GpuMat(dim, dim, CV_32FC3));

  std::vector<cv::cuda::GpuMat> normed_input(
    NUM_CUDA_STREAMS,
    cv::cuda::GpuMat(dim, dim, CV_32FC3));

  std::vector<double> task_times;
  std::vector<double> idle_times;
  while (true) {
    auto idle_start = now();
    // Wait for buffer to process
    EvalWorkEntry work_entry;
    args.eval_work.pop(work_entry);

    if (work_entry.work_item_index == -1) {
      break;
    }

    idle_times.push_back(nano_since(idle_start));

    auto start = now();

    const VideoWorkItem& work_item =
      args.work_items[work_entry.work_item_index];
    const VideoMetadata& metadata = args.metadata[work_item.video_index];

    size_t frame_size =
      av_image_get_buffer_size(AV_PIX_FMT_NV12,
                               metadata.width,
                               metadata.height,
                               1);

    // Resize net input blob for batch size
    const boost::shared_ptr<caffe::Blob<float>> data_blob{
      net->blob_by_name("data")};
    if (data_blob->shape(0) != GLOBAL_BATCH_SIZE) {
      data_blob->Reshape({
          GLOBAL_BATCH_SIZE, 3, net_info.input_size, net_info.input_size});
    }

    char* frame_buffer = args.frame_buffers[work_entry.buffer_index];

    int current_frame = work_item.start_frame;
    while (current_frame + GLOBAL_BATCH_SIZE < work_item.end_frame) {
      int frame_offset = current_frame - work_item.start_frame;

      float* net_input_buffer = net_input.mutable_gpu_data();

      // Process batch of frames
      for (int i = 0; i < GLOBAL_BATCH_SIZE; ++i) {
        int sid = i % NUM_CUDA_STREAMS;
        cv::cuda::Stream& cv_stream = cv_streams[sid];
        char* buffer = frame_buffer + frame_size * (i + frame_offset);
#ifdef HARDWARE_DECODE
        cv::cuda::GpuMat input_mat(
          metadata.height + metadata.height / 2,
          metadata.width,
          CV_8UC1,
          buffer);
#else
        cv::Mat cpu_mat(
          metadata.height + metadata.height / 2,
          metadata.width,
          CV_8UC1,
          buffer);
        input_mats[sid].upload(cpu_mat, cv_stream);
        cv::cuda::GpuMat& input_mat = input_mats[sid];
#endif
        convertNV12toRGBA(input_mat, rgba_mat[sid],
                          metadata.width, metadata.height,
                          cv_stream);
        cv::cuda::cvtColor(rgba_mat[sid], rgb_mat[sid], CV_RGBA2BGR, 0,
                           cv_stream);
        cv::cuda::resize(rgb_mat[sid], conv_input[sid], cv::Size(dim, dim),
                         0, 0, cv::INTER_LINEAR, cv_stream);
        conv_input[sid].convertTo(float_conv_input[sid], CV_32FC3, cv_stream);
        cv::cuda::subtract(float_conv_input[sid], mean_mat, normed_input[sid],
                           cv::noArray(), -1, cv_stream);
        cudaStream_t s = cv::cuda::StreamAccessor::getStream(cv_stream);
        CU_CHECK(cudaMemcpyAsync(
                   net_input_buffer + i * (dim * dim * 3),
                   normed_input[sid].data,
                   dim * dim * 3 * sizeof(float),
                   cudaMemcpyDeviceToDevice,
                   s));

        // For checking for proper encoding
        if (false && ((current_frame + i) % 512) == 0) {
          size_t image_size = metadata.width * metadata.height * 3;
          uint8_t* image_buff = new uint8_t[image_size];
          CU_CHECK(cudaMemcpy(image_buff, rgb_mat[sid].data, image_size,
                              cudaMemcpyDeviceToHost));
          JPEGWriter writer;
          writer.header(metadata.width, metadata.height, 3, JPEG::COLOR_RGB);
          std::vector<uint8_t*> rows(metadata.height);
          for (int i = 0; i < metadata.height; ++i) {
            rows[i] = image_buff + metadata.width * 3 * i;
          }
          std::string image_path =
            "frame" + std::to_string(current_frame + i) + ".jpg";
          writer.write(image_path, rows.begin());
          delete[] image_buff;
        }
      }

      CU_CHECK(cudaDeviceSynchronize());
      net->Forward({&net_input});

      // Save batch of frames
      current_frame += GLOBAL_BATCH_SIZE;
    }

    // Epilogue for processing less than a batch of frames
    if (current_frame < work_item.end_frame) {
      int batch_size = work_item.end_frame - current_frame;

      // Resize for our smaller batch size
      if (data_blob->shape(0) != batch_size) {
        data_blob->Reshape({
            batch_size, 3, net_info.input_size, net_info.input_size});
      }

      int frame_offset = current_frame - work_item.start_frame;

      // Process batch of frames
      caffe::Blob<float> net_input{batch_size, 3, dim, dim};

      float* net_input_buffer;
      net_input_buffer = net_input.mutable_gpu_data();

      for (int i = 0; i < batch_size; ++i) {
        int sid = i % NUM_CUDA_STREAMS;
        cv::cuda::Stream& cv_stream = cv_streams[sid];
        char* buffer = frame_buffer + frame_size * (i + frame_offset);
#ifdef HARDWARE_DECODE
        cv::cuda::GpuMat input_mat(
          metadata.height + metadata.height / 2,
          metadata.width,
          CV_8UC1,
          buffer);
#else
        cv::Mat cpu_mat(
          metadata.height + metadata.height / 2,
          metadata.width,
          CV_8UC1,
          buffer);
        input_mats[sid].upload(cpu_mat, cv_stream);
        cv::cuda::GpuMat& input_mat = input_mats[sid];
#endif
        convertNV12toRGBA(input_mat, rgba_mat[sid],
                          metadata.width, metadata.height,
                          cv_stream);
        cv::cuda::cvtColor(rgba_mat[sid], rgb_mat[sid], CV_RGBA2BGR, 0,
                           cv_stream);
        cv::cuda::resize(rgb_mat[sid], conv_input[sid], cv::Size(dim, dim),
                         0, 0, cv::INTER_LINEAR, cv_stream);
        conv_input[sid].convertTo(float_conv_input[sid], CV_32FC3, cv_stream);
        cv::cuda::subtract(float_conv_input[sid], mean_mat, normed_input[sid],
                           cv::noArray(), -1, cv_stream);
        cudaStream_t s = cv::cuda::StreamAccessor::getStream(cv_stream);
        CU_CHECK(cudaMemcpyAsync(
                   net_input_buffer + i * (dim * dim * 3),
                   normed_input[sid].data,
                   dim * dim * 3 * sizeof(float),
                   cudaMemcpyDeviceToDevice,
                   s));
      }

      CU_CHECK(cudaDeviceSynchronize());
      net->Forward({&net_input});

      // Save batch of frames
      current_frame += batch_size;
    }

    task_times.push_back(nano_since(start));

    LoadBufferEntry empty_buffer_entry;
    empty_buffer_entry.gpu_device_id = args.gpu_device_id;
    empty_buffer_entry.buffer_index = work_entry.buffer_index;
    args.empty_load_buffers.push(empty_buffer_entry);
  }

  delete net;

  double total_task_time = 0;
  for (double t : task_times) {
    total_task_time += t;
  }
  total_task_time /= 1000000; // convert from ns to ms
  double mean_task_time = total_task_time / task_times.size();
  double std_dev_task_time = 0;
  for (double t : task_times) {
    std_dev_task_time += std::pow(t / 1000000 - mean_task_time, 2);
  }
  std_dev_task_time = std::sqrt(std_dev_task_time / task_times.size());

  double total_idle_time = 0;
  for (double t : idle_times) {
    total_idle_time += t;
  }
  total_idle_time /= 1000000; // convert from ns to ms

  printf("(N/GPU: %d/%d) Evaluate thread finished. "
         "Total: %.3fms,  # Tasks: %lu, Mean: %.3fms, Std: %.3fms, "
         "Idle: %.3fms, Idle %: %3.2f\n",
         rank, args.gpu_device_id,
         total_task_time, task_times.size(), mean_task_time, std_dev_task_time,
         total_idle_time,
         total_idle_time / (total_idle_time + total_task_time));

  THREAD_RETURN_SUCCESS();
}

void startup(int argc, char** argv) {
  MPI_Init(&argc, &argv);
  av_register_all();
  FLAGS_minloglevel = 2;
  av_mutex = PTHREAD_MUTEX_INITIALIZER;
}

void shutdown() {
  MPI_Finalize();
}

int main(int argc, char **argv) {
  std::string video_paths_file;
  {
    po::variables_map vm;
    po::options_description desc("Allowed options");
    desc.add_options()
      ("help", "Produce help message")
      ("video_paths_file", po::value<std::string>()->required(),
       "File which contains paths to video files to process")
      ("gpus_per_node", po::value<int>(), "Number of GPUs per node")
      ("batch_size", po::value<int>(), "Neural Net input batch size")
      ("batches_per_work_item", po::value<int>(),
       "Number of batches in each work item")
      ("tasks_in_queue_per_gpu", po::value<int>(),
       "Number of tasks a node will try to maintain in the work queue per GPU")
      ("load_workers_per_node", po::value<int>(),
       "Number of worker threads processing load jobs per node");
    try {
      po::store(po::parse_command_line(argc, argv, desc), vm);
      po::notify(vm);

      if (vm.count("help")) {
        std::cout << desc << std::endl;
        return 1;
      }

      if (vm.count("gpus_per_node")) {
        GPUS_PER_NODE = vm["gpus_per_node"].as<int>();
      }
      if (vm.count("batch_size")) {
        GLOBAL_BATCH_SIZE = vm["batch_size"].as<int>();
      }
      if (vm.count("batches_per_work_item")) {
        BATCHES_PER_WORK_ITEM = vm["batches_per_work_item"].as<int>();
      }
      if (vm.count("tasks_in_queue_per_gpu")) {
        TASKS_IN_QUEUE_PER_GPU = vm["tasks_in_queue_per_gpu"].as<int>();
      }
      if (vm.count("load_workers_per_node")) {
        LOAD_WORKERS_PER_NODE = vm["load_workers_per_node"].as<int>();
      }

      video_paths_file = vm["video_paths_file"].as<std::string>();

    } catch (const po::required_option& e) {
      if (vm.count("help")) {
        std::cout << desc << std::endl;
        return 1;
      } else {
        throw e;
      }
    }
  }

  startup(argc, argv);

  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  int num_nodes;
  MPI_Comm_size(MPI_COMM_WORLD, &num_nodes);

  // Setup storage config
  StorageConfig* config =
    StorageConfig::make_disk_config(DB_PATH);
  StorageBackend* storage = StorageBackend::make_from_config(config);

  // Read in list of video paths
  std::vector<std::string> video_paths;
  {
    std::fstream fs(video_paths_file, std::fstream::in);
    while (fs) {
      std::string path;
      fs >> path;
      if (path.empty()) continue;
      video_paths.push_back(path);
    }
  }

  // Check if we have already preprocessed the videos
  bool all_preprocessed = true;
  for (const std::string& path : video_paths) {
    FileInfo video_info;
    StoreResult result =
      storage->get_file_info(processed_video_path(path), video_info);
    if (result == StoreResult::FileDoesNotExist) {
      all_preprocessed = false;
      // Preprocess video and then exit
      if (is_master(rank)) {
        log_ls.print("Video %s not processed yet. Processing now...\n",
                     path.c_str());
        preprocess_video(storage,
                         path,
                         processed_video_path(path),
                         metadata_path(path),
                         iframe_path(path));
      }
    }
  }
  if (all_preprocessed) {
    // Get video metadata for all videos for distributing with work items
    std::vector<VideoMetadata> video_metadata;
    for (const std::string& path : video_paths) {
      std::unique_ptr<RandomReadFile> metadata_file;
      exit_on_error(
        make_unique_random_read_file(storage,
                                     metadata_path(path),
                                     metadata_file));
      VideoMetadata metadata;
      (void) read_video_metadata(metadata_file.get(), 0, metadata);
      video_metadata.push_back(metadata);
    }

    // Break up videos and their frames into equal sized work items
    const int WORK_ITEM_SIZE = frames_per_work_item();
    std::vector<VideoWorkItem> work_items;
    for (size_t i = 0; i < video_paths.size(); ++i) {
      const VideoMetadata& meta = video_metadata[i];

      int32_t allocated_frames = 0;
      while (allocated_frames < meta.frames) {
        int32_t frames_to_allocate =
          std::min(WORK_ITEM_SIZE, meta.frames - allocated_frames);

        VideoWorkItem item;
        item.video_index = i;
        item.start_frame = allocated_frames;
        item.end_frame = allocated_frames + frames_to_allocate;
        work_items.push_back(item);

        allocated_frames += frames_to_allocate;
      }
    }
    if (is_master(rank)) {
      printf("Total work items: %lu\n", work_items.size());
    }

    // Setup shared resources for distributing work to processing threads
    Queue<LoadWorkEntry> load_work;
    Queue<LoadBufferEntry> empty_load_buffers;
    std::vector<Queue<EvalWorkEntry>> eval_work(GPUS_PER_NODE);

    // Allocate several buffers to hold the intermediate of an entire work item
    // to allow pipelining of load/eval
    // HACK(apoms): we are assuming that all videos have the same frame size
    // We should allocate the buffer in the load thread if we need to support
    // multiple sizes or analyze all the videos an allocate buffers for the
    // largest possible size
    size_t frame_size =
      av_image_get_buffer_size(AV_PIX_FMT_NV12,
                               video_metadata[0].width,
                               video_metadata[0].height,
                               1);
    size_t frame_buffer_size = frame_size * frames_per_work_item();
    const int LOAD_BUFFERS = TASKS_IN_QUEUE_PER_GPU;
    char*** gpu_frame_buffers = new char**[GPUS_PER_NODE];
    for (int gpu = 0; gpu < GPUS_PER_NODE; ++gpu) {
      CU_CHECK(cudaSetDevice(gpu));
      gpu_frame_buffers[gpu] = new char*[LOAD_BUFFERS];
      char** frame_buffers = gpu_frame_buffers[gpu];
      for (int i = 0; i < LOAD_BUFFERS; ++i) {
#ifdef HARDWARE_DECODE
        CU_CHECK(cudaMalloc(&frame_buffers[i], frame_buffer_size));
#else
        frame_buffers[i] = new char[frame_buffer_size];
#endif
        // Add the buffer index into the empty buffer queue so workers can
        // fill it to pass to the eval worker
        empty_load_buffers.emplace(LoadBufferEntry{gpu, i});
      }
    }

    // Setup load workers
    std::vector<LoadThreadArgs> load_thread_args;
    for (int i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
      // Retain primary context to use for decoder
#ifdef HARDWARE_DECODE
      std::vector<CUcontext> cuda_contexts(GPUS_PER_NODE);
      for (int gpu = 0; gpu < GPUS_PER_NODE; ++gpu) {
        CUD_CHECK(cuDevicePrimaryCtxRetain(&cuda_contexts[gpu], gpu));
      }
#endif

      // Create IO thread for reading and decoding data
      load_thread_args.emplace_back(LoadThreadArgs{
        // Uniform arguments
        video_paths,
        video_metadata,
        work_items,

        // Per worker arguments
        config,
#ifdef HARDWARE_DECODE
        cuda_contexts,
#endif

        // Queues
        load_work,
        empty_load_buffers,
        eval_work,

        // Buffers
        frame_buffer_size,
        gpu_frame_buffers,
      });
    }
    std::vector<pthread_t> load_threads(LOAD_WORKERS_PER_NODE);
    for (int i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
      pthread_create(&load_threads[i], NULL, load_video_thread,
                     &load_thread_args[i]);
    }

    // Setup evaluate workers
    std::vector<EvaluateThreadArgs> eval_thread_args;
    for (int i = 0; i < GPUS_PER_NODE; ++i) {
      int gpu_device_id = i;

      // Create eval thread for passing data through neural net
      eval_thread_args.emplace_back(EvaluateThreadArgs{
        // Uniform arguments
        video_metadata,
        work_items,

        // Per worker arguments
        gpu_device_id,

        // Queues
        eval_work[i],
        empty_load_buffers,

        // Buffers
        frame_buffer_size,
        gpu_frame_buffers[i],
      });
    }
    std::vector<pthread_t> eval_threads(GPUS_PER_NODE);
    for (int i = 0; i < GPUS_PER_NODE; ++i) {
      pthread_create(&eval_threads[i], NULL, evaluate_thread,
                     &eval_thread_args[i]);
    }

    // Push work into load queues
    if (is_master(rank)) {
      // Begin distributing work on master node
      int next_work_item_to_allocate = 0;
      // Wait for clients to ask for work
      while (next_work_item_to_allocate < static_cast<int>(work_items.size())) {
        if (next_work_item_to_allocate % 10 == 0) {
          printf("Work items left: %d\n",
                 static_cast<int>(work_items.size()) -
                 next_work_item_to_allocate);
        }
        // Check if we need to allocate work to our own processing thread
        int local_work = load_work.size();
        for (size_t i = 0; i < eval_work.size(); ++i) {
          local_work += eval_work[i].size();
        }
        if (local_work < GPUS_PER_NODE * TASKS_IN_QUEUE_PER_GPU) {
          LoadWorkEntry entry;
          entry.work_item_index = next_work_item_to_allocate++;
          load_work.push(entry);
          continue;
        }

        if (num_nodes > 1) {
          int more_work;
          MPI_Status status;
          MPI_Recv(&more_work, 1, MPI_INT,
                   MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
          int next_item = next_work_item_to_allocate++;
          MPI_Send(&next_item, 1, MPI_INT,
                   status.MPI_SOURCE, 0, MPI_COMM_WORLD);
        }
        std::this_thread::yield();
      }
      int workers_done = 1;
      while (workers_done < num_nodes) {
        int more_work;
        MPI_Status status;
        MPI_Recv(&more_work, 1, MPI_INT,
                 MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        int next_item = -1;
        MPI_Send(&next_item, 1, MPI_INT,
                 status.MPI_SOURCE, 0, MPI_COMM_WORLD);
        workers_done += 1;
        std::this_thread::yield();
      }
    } else {
      // Monitor amount of work left and request more when running low
      while (true) {
        int local_work = load_work.size();
        for (size_t i = 0; i < eval_work.size(); ++i) {
          local_work += eval_work[i].size();
        }
        if (local_work < GPUS_PER_NODE * TASKS_IN_QUEUE_PER_GPU) {
          // Request work when there is only a few unprocessed items left
          int more_work = true;
          MPI_Send(&more_work, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
          int next_item;
          MPI_Recv(&next_item, 1, MPI_INT, 0, MPI_ANY_TAG, MPI_COMM_WORLD,
                   MPI_STATUS_IGNORE);
          if (next_item == -1) {
            // No more work left
            break;
          } else {
            LoadWorkEntry entry;
            entry.work_item_index = next_item;
            load_work.push(entry);
          }
        }
        std::this_thread::yield();
      }
    }

    // Push sentinel work entries into queue to terminate load threads
    for (int i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
      LoadWorkEntry entry;
      entry.work_item_index = -1;
      load_work.push(entry);
    }

    for (int i = 0; i < LOAD_WORKERS_PER_NODE; ++i) {
      // Wait until load has finished
      void* result;
      int err = pthread_join(load_threads[i], &result);
      if (err != 0) {
        fprintf(stderr, "error in pthread_join of load thread\n");
        exit(EXIT_FAILURE);
      }
      free(result);

      // Cleanup
#ifdef HARDWARE_DECODE
      for (int gpu = 0; gpu < GPUS_PER_NODE; ++gpu) {
        CUD_CHECK(cuDevicePrimaryCtxRelease(gpu));
      }
#endif
    }

    // Push sentinel work entries into queue to terminate eval threads
    for (int i = 0; i < GPUS_PER_NODE; ++i) {
      EvalWorkEntry entry;
      entry.work_item_index = -1;
      eval_work[i].push(entry);
    }

    for (int i = 0; i < GPUS_PER_NODE; ++i) {
      // Wait until eval has finished
      void* result;
      int err = pthread_join(eval_threads[i], &result);
      if (err != 0) {
        fprintf(stderr, "error in pthread_join of eval thread\n");
        exit(EXIT_FAILURE);
      }
      free(result);
    }


    for (int gpu = 0; gpu < GPUS_PER_NODE; ++gpu) {
      char** frame_buffers = gpu_frame_buffers[gpu];
      for (int i = 0; i < LOAD_BUFFERS; ++i) {
#ifdef HARDWARE_DECODE
        CU_CHECK(cudaSetDevice(gpu));
        CU_CHECK(cudaFree(frame_buffers[i]));
#else
        delete[] frame_buffers[i];
#endif
      }
      delete[] frame_buffers;
    }
    delete[] gpu_frame_buffers;
  }

  // Cleanup
  delete storage;
  delete config;

  shutdown();

  return EXIT_SUCCESS;
}
