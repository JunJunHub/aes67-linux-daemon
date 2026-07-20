//  streamer.hpp
//
//  Copyright (c) 2019 2024 Andrea Bondavalli. All rights reserved.
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef _STREAMER_HPP_
#define _STREAMER_HPP_

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>
#include <alsa/asoundlib.h>
#include <faac.h>

#include "session_manager.hpp"

#ifdef _USE_NOISE_
#include "pcm_capture_service.hpp"
#endif

struct StreamerInfo {
  uint8_t status;
  uint16_t file_duration{0};
  uint8_t files_num{0};
  uint8_t player_buffer_files_num{0};
  uint8_t channels{0};
  uint8_t start_file_id{0};
  uint8_t current_file_id{0};
  uint32_t rate{0};
  std::string format;
};

struct StreamerLiveInfo {
  uint8_t sink_id{0};
  uint8_t file_id{0};
  uint8_t unwriteble{0};
};

class Streamer {
 public:
  static std::shared_ptr<Streamer> create(
      std::shared_ptr<SessionManager> session_manager,
      std::shared_ptr<Config> config);
  Streamer() = delete;
  Streamer(const Browser&) = delete;
  Streamer& operator=(const Browser&) = delete;

  bool init();
  bool terminate();

#ifdef _USE_NOISE_
  // PcmCaptureService 注入点（main.cpp 装配留 Spec3 1.11）。不改
  // Streamer::create 公开签名，避免上游 sync 冲突。
  void set_pcm_capture(std::shared_ptr<PcmCaptureService> pcm_capture) {
    pcm_capture_ = std::move(pcm_capture);
  }
#endif

  std::error_code get_info(const StreamSink& sink, StreamerInfo& info);
  std::error_code get_stream(const StreamSink& sink,
                             uint8_t file_id,
                             uint8_t& current_file_id,
                             uint8_t& start_file_id,
                             uint32_t& file_count,
                             std::string& out);

  std::error_code live_stream_init(const StreamSink& sink,
                                   const std::string& ip,
                                   int port);
  bool live_stream_wait(httplib::DataSink& httpSink,
                        const std::string& ip,
                        int port);

 protected:
  explicit Streamer(std::shared_ptr<SessionManager> session_manager,
                    std::shared_ptr<Config> config)
      : session_manager_(session_manager), config_(config){};

 private:
  constexpr static const char device_name[] = "plughw:RAVENNA";
  constexpr static snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;

  bool pcm_xrun();
  bool pcm_suspend();
  ssize_t pcm_read(uint8_t* data, size_t rcount);

#ifdef _USE_NOISE_
  // FrameProvider 回调入口：PcmCaptureService 分发的 PCM 帧喂入既有 AAC 编码
  // 管线（替代 snd_pcm_readi 路径）。
  void on_pcm_frame_from_capture(const uint8_t* pcm,
                                 size_t frame_count,
                                 uint8_t channels);
#endif

  bool on_ptp_status_change(const std::string& status);
  bool on_sink_add(uint8_t id);
  bool on_sink_remove(uint8_t id);
  bool start_capture();
  bool stop_capture();
  bool setup_codec(const StreamSink& sink);
  void open_files(uint8_t files_id);
  void close_files(uint8_t files_id);
  void save_files(uint8_t files_id);

#ifdef _USE_NOISE_
  std::shared_ptr<PcmCaptureService> pcm_capture_;
  PcmCaptureService::ProviderToken pcm_token_{0};
#endif

  std::shared_ptr<SessionManager> session_manager_;
  std::shared_ptr<Config> config_;
  snd_pcm_uframes_t chunk_samples_{0};
  size_t bytes_per_frame_{0};
  uint16_t file_duration_{1};
  uint8_t files_num_{8};
  uint8_t player_buffer_files_num_{1};
  size_t buffer_samples_{0};
  std::unordered_map<uint8_t, size_t> total_sink_samples_;
  uint32_t buffer_offset_{0};
  std::unordered_map<uint8_t, std::shared_mutex> streams_mutex_;
  std::unordered_map<uint8_t, std::stringstream> tmp_streams_;
  std::map<std::pair<uint8_t, uint8_t>, std::stringstream> output_streams_;
  std::unordered_map<uint8_t, uint32_t> output_ids_;
  uint32_t file_counter_{0};
  std::atomic<uint8_t> file_id_{0};
  std::unique_ptr<uint8_t[]> buffer_;
  std::unordered_map<uint8_t, std::unique_ptr<uint8_t[]> > out_buffer_;
  std::unordered_map<uint8_t, uint32_t> out_buffer_size_{0};
  uint8_t channels_{8};
  uint32_t rate_{0};
  std::future<bool> res_;
  snd_pcm_t* capture_handle_;
  std::atomic_bool running_{false};
  std::unordered_map<uint8_t, faacEncHandle> faac_;
  std::unordered_map<uint8_t, std::mutex> faac_mutex_;
  std::unordered_map<uint8_t, unsigned long> codec_in_samples_;
  std::unordered_map<uint8_t, unsigned long> codec_out_buffer_size_;
  std::map<std::pair<std::string, int>, StreamerLiveInfo> liveInfos_;
};

#endif
