//  streamer.cpp
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

#include <boost/algorithm/string.hpp>
#include <cstring>

#include "utils.hpp"
#include "streamer.hpp"

std::shared_ptr<Streamer> Streamer::create(
    std::shared_ptr<SessionManager> session_manager,
    std::shared_ptr<Config> config) {
  // no need to be thread-safe here
  static std::weak_ptr<Streamer> instance;
  if (auto ptr = instance.lock()) {
    return ptr;
  }
  auto ptr = std::shared_ptr<Streamer>(new Streamer(session_manager, config));
  instance = ptr;
  return ptr;
}

bool Streamer::init() {
  BOOST_LOG_TRIVIAL(info) << "Streamer: init";
#ifdef _USE_NOISE_
  // WITH_NOISE：PTP observer 由 PcmCaptureService 负责，Streamer 改注册
  // FrameProvider 接收 PcmCaptureService 分发的 ALSA period 帧。
  if (pcm_capture_) {
    pcm_token_ = pcm_capture_->register_provider(
        [this](const uint8_t* pcm, size_t frame_count, uint8_t channels,
               uint32_t /*rate*/) {
          this->on_pcm_frame_from_capture(pcm, frame_count, channels);
        });
  }
#else
  session_manager_->add_ptp_status_observer(
      std::bind(&Streamer::on_ptp_status_change, this, std::placeholders::_1));
  session_manager_->add_sink_observer(
      SessionManager::SinkObserverType::add_sink,
      std::bind(&Streamer::on_sink_add, this, std::placeholders::_1));
  session_manager_->add_sink_observer(
      SessionManager::SinkObserverType::remove_sink,
      std::bind(&Streamer::on_sink_remove, this, std::placeholders::_1));

  running_ = false;

  PTPStatus status;
  session_manager_->get_ptp_status(status);
  on_ptp_status_change(status.status);
#endif
  return true;
}

bool Streamer::on_sink_add(uint8_t id) {
  return true;
}

bool Streamer::on_sink_remove(uint8_t id) {
  if (faac_[id]) {
    std::unique_lock faac_lock(faac_mutex_[id]);
    faacEncClose(faac_[id]);
    faac_[id] = 0;
  }
  total_sink_samples_[id] = 0;
  return true;
}

bool Streamer::on_ptp_status_change(const std::string& status) {
  BOOST_LOG_TRIVIAL(info) << "Streamer: new ptp status " << status;
  if (status == "locked") {
    return start_capture();
  }
  if (status == "unlocked") {
    return stop_capture();
  }
  return true;
}

#ifndef timersub
#define timersub(a, b, result)                       \
  do {                                               \
    (result)->tv_sec = (a)->tv_sec - (b)->tv_sec;    \
    (result)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
    if ((result)->tv_usec < 0) {                     \
      --(result)->tv_sec;                            \
      (result)->tv_usec += 1000000;                  \
    }                                                \
  } while (0)
#endif

bool Streamer::pcm_xrun() {
  snd_pcm_status_t* status;
  int res;
  snd_pcm_status_alloca(&status);
  if ((res = snd_pcm_status(capture_handle_, status)) < 0) {
    BOOST_LOG_TRIVIAL(error)
        << "streamer:: pcm_xrun status error: " << snd_strerror(res);
    return false;
  }
  if (snd_pcm_status_get_state(status) == SND_PCM_STATE_XRUN) {
    struct timeval now, diff, tstamp;
    gettimeofday(&now, 0);
    snd_pcm_status_get_trigger_tstamp(status, &tstamp);
    timersub(&now, &tstamp, &diff);
    BOOST_LOG_TRIVIAL(error)
        << "streamer:: pcm_xrun overrun!!! (at least "
        << diff.tv_sec * 1000 + diff.tv_usec / 1000.0 << " ms long";

    if ((res = snd_pcm_prepare(capture_handle_)) < 0) {
      BOOST_LOG_TRIVIAL(error)
          << "streamer:: pcm_xrun prepare error: " << snd_strerror(res);
      return false;
    }
    return true; /* ok, data should be accepted again */
  }
  if (snd_pcm_status_get_state(status) == SND_PCM_STATE_DRAINING) {
    BOOST_LOG_TRIVIAL(error)
        << "streamer:: capture stream format change? attempting recover...";
    if ((res = snd_pcm_prepare(capture_handle_)) < 0) {
      BOOST_LOG_TRIVIAL(error)
          << "streamer:: pcm_xrun xrun(DRAINING) error: " << snd_strerror(res);
      return false;
    }
    return true;
  }
  BOOST_LOG_TRIVIAL(error) << "streamer:: read/write error, state = "
                           << snd_pcm_state_name(
                                  snd_pcm_status_get_state(status));
  return false;
}

/* I/O suspend handler */
bool Streamer::pcm_suspend() {
  int res;
  BOOST_LOG_TRIVIAL(info) << "streamer:: Suspended. Trying resume. ";
  while ((res = snd_pcm_resume(capture_handle_)) == -EAGAIN)
    sleep(1); /* wait until suspend flag is released */
  if (res < 0) {
    BOOST_LOG_TRIVIAL(error) << "streamer:: Failed. Restarting stream. ";
    if ((res = snd_pcm_prepare(capture_handle_)) < 0) {
      BOOST_LOG_TRIVIAL(error)
          << "streamer:: suspend: prepare error:  " << snd_strerror(res);
      return false;
    }
  }
  return true;
}

#ifdef _USE_NOISE_
// FrameProvider 回调：收到一个 ALSA period 的全通道交错 PCM，
// 喂入既有 AAC 编码管线（替代 snd_pcm_readi 路径）。
// 复用 save_files / close_files / open_files / file 轮转逻辑。
void Streamer::on_pcm_frame_from_capture(const uint8_t* pcm,
                                         size_t frame_count,
                                         uint8_t /*channels*/) {
  if (!running_ || pcm == nullptr)
    return;
  // channels 与 setup_codec 时的 channels_ 一致（PcmCaptureService
  // 全通道分发）。 chunk_samples_ == kPeriodSamples == 6144，与
  // PcmCaptureService 对齐。
  const size_t bytes = frame_count * bytes_per_frame_;
  // 缓冲满：切文件（复用 start_capture 的轮转逻辑）
  if (buffer_offset_ * bytes_per_frame_ + bytes >
      buffer_samples_ * bytes_per_frame_) {
    close_files(file_id_);
    file_id_ = (file_id_ + 1) % files_num_;
    file_counter_++;
    buffer_offset_ = 0;
    open_files(file_id_);
  }
  std::memcpy(buffer_.get() + buffer_offset_ * bytes_per_frame_, pcm, bytes);
  save_files(file_id_);
  buffer_offset_ += frame_count;
}
#endif

ssize_t Streamer::pcm_read(uint8_t* data, size_t rcount) {
  ssize_t r;
  size_t count = rcount;

  if (count != chunk_samples_) {
    count = chunk_samples_;
  }

  while (count > 0) {
    r = snd_pcm_readi(capture_handle_, data, count);
    if (r == -EAGAIN || (r >= 0 && (size_t)r < count)) {
      if (!running_)
        return -1;
      snd_pcm_wait(capture_handle_, 1000);
    } else if (r == -EPIPE) {
      if (!pcm_xrun())
        return -1;
    } else if (r == -ESTRPIPE) {
      if (!pcm_suspend())
        return -1;
    } else if (r < 0) {
      BOOST_LOG_TRIVIAL(error) << "streamer:: read error: " << snd_strerror(r);
      return -1;
    }
    if (r > 0) {
      count -= r;
      data += r * bytes_per_frame_;
    }
  }
  return rcount;
}

bool Streamer::start_capture() {
  if (running_)
    return true;

  BOOST_LOG_TRIVIAL(info) << "Streamer: starting audio capture ... ";
#ifdef _USE_NOISE_
  // WITH_NOISE：ALSA capture 由 PcmCaptureService 独占，Streamer 不 open。
  // 仅初始化 AAC 编码参数与 buffer（复用既有 setup，不启 async capture loop）。
  rate_ = config_->get_sample_rate();
  channels_ = config_->get_streamer_channels();
  files_num_ = config_->get_streamer_files_num();
  file_duration_ = config_->get_streamer_file_duration();
  player_buffer_files_num_ = config_->get_streamer_player_buffer_files_num();
  chunk_samples_ = 6144;
  bytes_per_frame_ = snd_pcm_format_physical_width(format) * channels_ / 8;
  buffer_samples_ = rate_ * file_duration_ / chunk_samples_ * chunk_samples_;
  buffer_.reset(new uint8_t[buffer_samples_ * bytes_per_frame_]);
  if (buffer_ == nullptr)
    return false;
  buffer_offset_ = 0;
  total_sink_samples_.clear();
  file_id_ = 0;
  file_counter_ = 0;
  running_ = true;
  open_files(file_id_);
  return true;
#else
  int err;
  if ((err = snd_pcm_open(&capture_handle_, device_name, SND_PCM_STREAM_CAPTURE,
                          SND_PCM_NONBLOCK)) < 0) {
    BOOST_LOG_TRIVIAL(fatal) << "streamer:: cannot open audio device "
                             << device_name << " : " << snd_strerror(err);
    return false;
  }

  snd_pcm_hw_params_t* hw_params;
  if ((err = snd_pcm_hw_params_malloc(&hw_params)) < 0) {
    BOOST_LOG_TRIVIAL(fatal)
        << "streamer:: cannot allocate hardware parameter structure: "
        << snd_strerror(err);
    return false;
  }

  if ((err = snd_pcm_hw_params_any(capture_handle_, hw_params)) < 0) {
    BOOST_LOG_TRIVIAL(fatal)
        << "streamer:: cannot initialize hardware parameter structure: "
        << snd_strerror(err);
    return false;
  }

  if ((err = snd_pcm_hw_params_set_access(capture_handle_, hw_params,
                                          SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
    BOOST_LOG_TRIVIAL(fatal)
        << "streamer:: cannot set access type: " << snd_strerror(err);
    return false;
  }

  if ((err = snd_pcm_hw_params_set_format(capture_handle_, hw_params, format)) <
      0) {
    BOOST_LOG_TRIVIAL(fatal)
        << "streamer:: cannot set sample format: " << snd_strerror(err);
    return false;
  }

  rate_ = config_->get_sample_rate();
  if ((err = snd_pcm_hw_params_set_rate_near(capture_handle_, hw_params, &rate_,
                                             0)) < 0) {
    BOOST_LOG_TRIVIAL(fatal)
        << "streamer:: cannot set sample rate: " << snd_strerror(err);
    return false;
  }

  channels_ = config_->get_streamer_channels();
  if ((err = snd_pcm_hw_params_set_channels(capture_handle_, hw_params,
                                            channels_)) < 0) {
    BOOST_LOG_TRIVIAL(fatal)
        << "streamer:: cannot set channel count: " << snd_strerror(err);
    return false;
  }

  files_num_ = config_->get_streamer_files_num();
  file_duration_ = config_->get_streamer_file_duration();
  player_buffer_files_num_ = config_->get_streamer_player_buffer_files_num();

  if ((err = snd_pcm_hw_params(capture_handle_, hw_params)) < 0) {
    BOOST_LOG_TRIVIAL(fatal)
        << "streamer:: cannot set parameters: " << snd_strerror(err);
    return false;
  }

  snd_pcm_hw_params_get_period_size(hw_params, &chunk_samples_, 0);
  chunk_samples_ = 6144;  // AAC 6 channels input
  bytes_per_frame_ = snd_pcm_format_physical_width(format) * channels_ / 8;

  snd_pcm_hw_params_free(hw_params);

  if ((err = snd_pcm_prepare(capture_handle_)) < 0) {
    BOOST_LOG_TRIVIAL(fatal)
        << "streamer:: cannot prepare audio interface for use: "
        << snd_strerror(err);
    return false;
  }

  buffer_samples_ = rate_ * file_duration_ / chunk_samples_ * chunk_samples_;
  BOOST_LOG_TRIVIAL(info) << "streamer: buffer_samples " << buffer_samples_;
  buffer_.reset(new uint8_t[buffer_samples_ * bytes_per_frame_]);
  if (buffer_ == nullptr) {
    BOOST_LOG_TRIVIAL(fatal) << "streamer: cannot allocate audio buffer";
    return false;
  }

  buffer_offset_ = 0;
  total_sink_samples_.clear();
  file_id_ = 0;
  file_counter_ = 0;
  running_ = true;

  open_files(file_id_);

  /* start capturing on a separate thread */
  res_ = std::async(std::launch::async, [&]() {
    BOOST_LOG_TRIVIAL(debug)
        << "streamer: audio capture loop start, chunk_samples_ = "
        << chunk_samples_;
    while (running_) {
      if ((pcm_read(buffer_.get() + buffer_offset_ * bytes_per_frame_,
                    chunk_samples_)) < 0) {
        break;
      }

      save_files(file_id_);
      buffer_offset_ += chunk_samples_;

      /* check id buffer is full */
      if (buffer_offset_ + chunk_samples_ > buffer_samples_) {
        close_files(file_id_);
        /* increase file id */
        file_id_ = (file_id_ + 1) % files_num_;
        file_counter_++;
        buffer_offset_ = 0;

        open_files(file_id_);
      }
    }
    BOOST_LOG_TRIVIAL(debug) << "streamer: audio capture loop end";
    return true;
  });

  return true;
#endif
}

void Streamer::open_files(uint8_t files_id) {
  BOOST_LOG_TRIVIAL(debug) << "streamer: opening files with id "
                           << std::to_string(files_id) << " ...";
  for (const auto& sink : session_manager_->get_sinks()) {
    tmp_streams_[sink.id].str("");
    std::unique_lock faac_lock(faac_mutex_[sink.id]);
    if (!faac_[sink.id]) {
      setup_codec(sink);
    }
  }
}

void Streamer::save_files(uint8_t files_id) {
  auto sample_size = bytes_per_frame_ / channels_;

  for (const auto& sink : session_manager_->get_sinks()) {
    total_sink_samples_[sink.id] += chunk_samples_;
    for (size_t offset = 0; offset < chunk_samples_; offset++) {
      for (uint16_t ch : sink.map) {
        auto in = buffer_.get() + (buffer_offset_ + offset) * bytes_per_frame_ +
                  ch * sample_size;
        std::copy(in, in + sample_size,
                  std::ostream_iterator<uint8_t>(tmp_streams_[sink.id]));
      }
    }
  }
}

bool Streamer::setup_codec(const StreamSink& sink) {
  /* open and setup the encoder */
  faac_[sink.id] = faacEncOpen(config_->get_sample_rate(), sink.map.size(),
                               &codec_in_samples_[sink.id],
                               &codec_out_buffer_size_[sink.id]);
  if (!faac_[sink.id]) {
    BOOST_LOG_TRIVIAL(fatal) << "streamer:: cannot open codec";
    return false;
  }
  BOOST_LOG_TRIVIAL(debug) << "streamer: codec samples in "
                           << codec_in_samples_[sink.id] << " out buffer size "
                           << codec_out_buffer_size_[sink.id];
  faacEncConfigurationPtr faac_cfg;
  /* check faac version */
  faac_cfg = faacEncGetCurrentConfiguration(faac_[sink.id]);
  if (!faac_cfg) {
    BOOST_LOG_TRIVIAL(fatal) << "streamer:: cannot get codec configuration";
    return false;
  }

  faac_cfg->aacObjectType = LOW;
  faac_cfg->mpegVersion = MPEG4;
  faac_cfg->useTns = 0;
  faac_cfg->useLfe = sink.map.size() > 6 ? 1 : 0;
  faac_cfg->shortctl = SHORTCTL_NORMAL;
  faac_cfg->allowMidside = 2;
  faac_cfg->bitRate = 64000 / sink.map.size();
  // faac_cfg->bandWidth = 18000;
  // faac_cfg->quantqual = 50;
  // faac_cfg->pnslevel = 4;
  // faac_cfg->jointmode = JOINT_MS;
  faac_cfg->outputFormat = 1;
  faac_cfg->inputFormat = FAAC_INPUT_16BIT;

  if (!faacEncSetConfiguration(faac_[sink.id], faac_cfg)) {
    BOOST_LOG_TRIVIAL(fatal) << "streamer:: cannot set codec configuration";
    return false;
  }

  out_buffer_size_[sink.id] = 0;
  return true;
}

void Streamer::close_files(uint8_t files_id) {
  uint16_t sample_size = bytes_per_frame_ / channels_;

  std::list<std::future<bool> > ress;
  for (const auto& sink : session_manager_->get_sinks()) {
    ress.emplace_back(std::async(std::launch::async, [=]() {
      uint32_t out_len = 0;
      {
        std::unique_lock faac_lock(faac_mutex_[sink.id]);
        if (!faac_[sink.id])
          return false;

        auto codec_in_samples = codec_in_samples_[sink.id];
        uint32_t out_size = codec_out_buffer_size_[sink.id] * buffer_samples_ *
                            sink.map.size() / codec_in_samples;

        if (out_size > out_buffer_size_[sink.id]) {
          out_buffer_[sink.id].reset(new uint8_t[out_size]);
          if (out_buffer_[sink.id] == nullptr) {
            BOOST_LOG_TRIVIAL(fatal)
                << "streamer: cannot allocate output buffer";
            return false;
          }
          out_buffer_size_[sink.id] = out_size;
        }

        uint32_t in_samples = 0;
        bool end = false;
        while (!end) {
          if (in_samples + codec_in_samples >=
              buffer_samples_ * sink.map.size()) {
            uint16_t diff = buffer_samples_ * sink.map.size() - in_samples;
            codec_in_samples = diff;
            end = true;
          }

          auto ret = faacEncEncode(
              faac_[sink.id],
              (int32_t*)(tmp_streams_[sink.id].str().c_str() +
                         in_samples * sample_size),
              codec_in_samples, out_buffer_[sink.id].get() + out_len,
              codec_out_buffer_size_[sink.id]);
          if (ret < 0) {
            BOOST_LOG_TRIVIAL(error)
                << "streamer: cannot encode file id "
                << std::to_string(files_id) << " for sink id "
                << std::to_string(sink.id);
            return false;
          }

          in_samples += codec_in_samples;
          out_len += ret;
        }
      }
      std::unique_lock streams_lock(streams_mutex_[sink.id]);
      output_streams_[std::make_pair(sink.id, files_id)].str("");
      std::copy(out_buffer_[sink.id].get(),
                out_buffer_[sink.id].get() + out_len,
                std::ostream_iterator<uint8_t>(
                    output_streams_[std::make_pair(sink.id, files_id)]));
      output_ids_[files_id] = file_counter_;
      return true;
    }));
  }

  for (auto& res : ress) {
    (void)res.get();
  }
}

bool Streamer::stop_capture() {
  if (!running_)
    return true;

  BOOST_LOG_TRIVIAL(info) << "streamer: stopping audio capture ... ";
  running_ = false;
#ifndef _USE_NOISE_
  bool ret = res_.get();
#else
  // WITH_NOISE：无 async capture loop（帧由 PcmCaptureService 推送）
  bool ret = true;
#endif
  for (const auto& sink : session_manager_->get_sinks()) {
    if (faac_[sink.id]) {
      faacEncClose(faac_[sink.id]);
      faac_[sink.id] = 0;
    }
  }
#ifndef _USE_NOISE_
  snd_pcm_close(capture_handle_);
#endif
  return ret;
}

bool Streamer::terminate() {
  BOOST_LOG_TRIVIAL(info) << "streamer: terminating ... ";
#ifdef _USE_NOISE_
  // WITH_NOISE：注销 FrameProvider（init() 注册的），停止接收 PcmCaptureService
  // 分发的帧。stop_capture() 关闭 faac 编码器。
  if (pcm_capture_ && pcm_token_) {
    pcm_capture_->unregister_provider(pcm_token_);
    pcm_token_ = 0;
  }
#endif
  return stop_capture();
}

std::error_code Streamer::get_info(const StreamSink& sink, StreamerInfo& info) {
  if (!running_) {
    BOOST_LOG_TRIVIAL(warning) << "streamer:: not running";
    return std::error_code{DaemonErrc::streamer_not_running};
  }

  for (uint16_t ch : sink.map) {
    if (ch >= channels_) {
      BOOST_LOG_TRIVIAL(error) << "streamer:: channel is not captured for sink "
                               << std::to_string(sink.id);
      return std::error_code{DaemonErrc::streamer_invalid_ch};
    }
  }

  if (total_sink_samples_[sink.id] < buffer_samples_ * (files_num_ - 1)) {
    BOOST_LOG_TRIVIAL(warning)
        << "streamer:: not enough samples buffered for sink "
        << std::to_string(sink.id);
    return std::error_code{DaemonErrc::streamer_retry_later};
  }

  auto file_id = file_id_.load();
  uint8_t start_file_id = (file_id + files_num_ / 2) % files_num_;

  switch (format) {
    case SND_PCM_FORMAT_S16_LE:
      info.format = "s16";
      break;
    case SND_PCM_FORMAT_S24_3LE:
      info.format = "s24";
      break;
    case SND_PCM_FORMAT_S32_LE:
      info.format = "s32";
      break;
    default:
      info.format = "invalid";
      break;
  }

  info.files_num = files_num_;
  info.file_duration = file_duration_;
  info.player_buffer_files_num = player_buffer_files_num_;
  info.rate = config_->get_sample_rate();
  info.channels = sink.map.size();
  info.start_file_id = start_file_id;
  info.current_file_id = file_id;

  BOOST_LOG_TRIVIAL(debug) << "streamer:: returning position "
                           << std::to_string(file_id);
  return std::error_code{};
}

std::error_code Streamer::get_stream(const StreamSink& sink,
                                     uint8_t files_id,
                                     uint8_t& current_file_id,
                                     uint8_t& start_file_id,
                                     uint32_t& file_counter,
                                     std::string& out) {
  if (!running_) {
    BOOST_LOG_TRIVIAL(warning) << "streamer:: not running";
    return std::error_code{DaemonErrc::streamer_not_running};
  }

  for (uint16_t ch : sink.map) {
    if (ch >= channels_) {
      BOOST_LOG_TRIVIAL(error) << "streamer:: channel is not captured for sink "
                               << std::to_string(sink.id);
      return std::error_code{DaemonErrc::streamer_invalid_ch};
    }
  }

  if (total_sink_samples_[sink.id] < buffer_samples_ * (files_num_ - 1)) {
    BOOST_LOG_TRIVIAL(warning)
        << "streamer:: not enough samples buffered for sink "
        << std::to_string(sink.id);
    return std::error_code{DaemonErrc::streamer_retry_later};
  }

  current_file_id = file_id_.load();
  start_file_id = (current_file_id + files_num_ / 2) % files_num_;
  if (files_id == current_file_id) {
    BOOST_LOG_TRIVIAL(error)
        << "streamer: requesting current file id " << std::to_string(files_id);
  }
  std::shared_lock streams_lock(streams_mutex_[sink.id]);
  out = output_streams_[std::make_pair(sink.id, files_id)].str();
  file_counter = output_ids_[files_id];
  return std::error_code{};
}

std::error_code Streamer::live_stream_init(const StreamSink& sink,
                                           const std::string& ip,
                                           int port) {
  if (!running_) {
    return std::error_code{DaemonErrc::streamer_not_running};
  }

  for (uint16_t ch : sink.map) {
    if (ch >= channels_) {
      return std::error_code{DaemonErrc::streamer_invalid_ch};
    }
  }

  if (total_sink_samples_[sink.id] < buffer_samples_ * (files_num_ - 1)) {
    return std::error_code{DaemonErrc::streamer_retry_later};
  }

  StreamerLiveInfo info;
  info.unwriteble = 0;
  info.sink_id = sink.id;
  info.file_id = (file_id_.load() + 1) % files_num_;
  liveInfos_[make_pair(ip, port)] = info;

  BOOST_LOG_TRIVIAL(info) << "streamer:: sink " << std::to_string(info.sink_id)
                          << " live started on remote " << ip << ":" << port;
  return std::error_code{};
}

bool Streamer::live_stream_wait(httplib::DataSink& httpSink,
                                const std::string& ip,
                                int port) {
  if (liveInfos_.find(make_pair(ip, port)) == liveInfos_.end()) {
    BOOST_LOG_TRIVIAL(error) << "streamer:: live sink with remote " << ip << ":"
                             << port << " not started, stopping ..";
    return false;
  }
  auto& info = liveInfos_[make_pair(ip, port)];
  while (info.file_id == file_id_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }

  if (httpSink.is_writable()) {
    std::shared_lock streams_lock(streams_mutex_[info.sink_id]);
    auto buf =
        output_streams_[std::make_pair(info.sink_id, info.file_id)].str();
    streams_lock.unlock();

    BOOST_LOG_TRIVIAL(debug)
        << "streamer:: live sink " << std::to_string(info.sink_id)
        << " sending file " << int(info.file_id) << " " << buf.length()
        << " bytes to " << ip << ":" << port;

    httpSink.write(buf.data(), buf.length());
    info.file_id = (info.file_id + 1) % files_num_;
    info.unwriteble = 0;
  } else {
    if (++info.unwriteble >= (files_num_ / 2)) {
      BOOST_LOG_TRIVIAL(warning)
          << "streamer:: live sink " << std::to_string(info.sink_id)
          << " remote " << ip << ":" << port << " not writable, stopping ..";
      liveInfos_.erase(make_pair(ip, port));
      return false;
    }
  }
  return true;
}

#ifdef _USE_NOISE_
// Spec3 Task 6：三路 AAC 编码 - /denoised /noise 路由入口（arch §4.4/§5.2）。
// 从 NoiseManager::get_denoise_output(sink_id) 拿 front 缓冲（previous period
// 的 DenoiseOutput），取 denoised 或 noise 通道的 float 样本，转 S16 后经
// faac 编码为 ADTS AAC 返回。
//   denoised=true  -> out->denoised（降噪后 PCM）
//   denoised=false -> out->noise（噪声分量 = original - denoised）
// sensor 不存在 / denoise 关 -> get_denoise_output 返回 nullptr ->
// error（404）。 线程安全：HTTP 控制线程调用，front 缓冲 cross-thread
// 读安全（RT 写 back， on_period_end swap front/back，arch §4.4 约束1）。
std::error_code Streamer::encode_denoise_aac(uint8_t sink_id,
                                             bool denoised,
                                             std::string& out) {
  if (!noise_manager_) {
    return std::error_code{DaemonErrc::streamer_not_running};
  }
  const noise::DenoiseOutput* dout =
      noise_manager_->get_denoise_output(sink_id);
  if (dout == nullptr || dout->frame_count == 0) {
    return std::error_code{DaemonErrc::streamer_not_running};
  }
  const float* src = denoised ? dout->denoised : dout->noise;
  if (src == nullptr) {
    return std::error_code{DaemonErrc::streamer_not_running};
  }

  // float [-1,1] -> S16 LE
  size_t n = dout->frame_count;
  std::vector<int16_t> s16(n);
  for (size_t i = 0; i < n; ++i) {
    float v = src[i];
    if (v > 1.0f)
      v = 1.0f;
    if (v < -1.0f)
      v = -1.0f;
    s16[i] = static_cast<int16_t>(v * 32767.0f);
  }

  // 临时 faac 编码器（1 通道，config 采样率）。
  unsigned long in_samples = 0;
  unsigned long out_buf_size = 0;
  faacEncHandle enc =
      faacEncOpen(config_->get_sample_rate(), 1, &in_samples, &out_buf_size);
  if (!enc) {
    BOOST_LOG_TRIVIAL(error)
        << "streamer:: encode_denoise_aac: faacEncOpen failed";
    return std::error_code{DaemonErrc::streamer_not_running};
  }
  faacEncConfigurationPtr faac_cfg = faacEncGetCurrentConfiguration(enc);
  if (faac_cfg) {
    faac_cfg->aacObjectType = LOW;
    faac_cfg->mpegVersion = MPEG4;
    faac_cfg->useTns = 0;
    faac_cfg->useLfe = 0;
    faac_cfg->shortctl = SHORTCTL_NORMAL;
    faac_cfg->allowMidside = 2;
    faac_cfg->bitRate = 64000;
    faac_cfg->outputFormat = 1;  // ADTS
    faac_cfg->inputFormat = FAAC_INPUT_16BIT;
    faacEncSetConfiguration(enc, faac_cfg);
  }

  // faac 需 in_samples 个样本/帧。front 缓冲通常 480 样本 < 1024（AAC-LC），
  // 补零到 in_samples 后一次编码。
  std::vector<int16_t> buf(in_samples, 0);
  size_t copy_n = std::min(n, static_cast<size_t>(in_samples));
  std::copy(s16.data(), s16.data() + copy_n, buf.data());

  std::vector<uint8_t> out_buf(out_buf_size);
  // faac 首次 encode 可能返回 0（内部 look-ahead 缓冲未满）。
  // 多次喂同一帧数据直到产出 AAC（最多 5 次，同 T8 E2E 测试模式）。
  int ret = 0;
  for (int i = 0; i < 5 && ret <= 0; ++i) {
    ret = faacEncEncode(enc, reinterpret_cast<int32_t*>(buf.data()), in_samples,
                        out_buf.data(), out_buf_size);
  }
  faacEncClose(enc);

  if (ret < 0) {
    BOOST_LOG_TRIVIAL(error)
        << "streamer:: encode_denoise_aac: faacEncEncode failed";
    return std::error_code{DaemonErrc::streamer_not_running};
  }
  out.assign(reinterpret_cast<const char*>(out_buf.data()), ret);
  return std::error_code{};
}

// Spec4 T2：PCM 直通 - float [-1,1] -> S16 LE（arch §5.2 + D-S4.6）。
// 与 encode_denoise_aac 的 float->S16 转换一致（v * 32767.0f + clamp [-1,1]）。
// 输出 16-bit signed LE interleaved，skip faac 编码。
static void float_to_s16_le(const float* src, size_t n, std::string& out) {
  out.resize(n * sizeof(int16_t));
  int16_t* dst = reinterpret_cast<int16_t*>(out.data());
  for (size_t i = 0; i < n; ++i) {
    float v = src[i];
    if (v > 1.0f)
      v = 1.0f;
    if (v < -1.0f)
      v = -1.0f;
    dst[i] = static_cast<int16_t>(v * 32767.0f);
  }
}

// Spec4 T2：原始路 PCM 直通 - 取 DenoiseOutput.original（arch §3.4 L649
// 统一三路接口）。
std::error_code Streamer::encode_original_pcm(uint8_t sink_id,
                                              std::string& out) {
  if (!noise_manager_) {
    return std::error_code{DaemonErrc::streamer_not_running};
  }
  const noise::DenoiseOutput* dout =
      noise_manager_->get_denoise_output(sink_id);
  if (dout == nullptr || dout->frame_count == 0 || dout->original == nullptr) {
    return std::error_code{DaemonErrc::streamer_not_running};
  }
  float_to_s16_le(dout->original, dout->frame_count, out);
  return std::error_code{};
}

// Spec4 T2：降噪/噪声路 PCM 直通 - 取 DenoiseOutput.denoised 或 .noise。
std::error_code Streamer::encode_denoise_pcm(uint8_t sink_id,
                                             bool denoised,
                                             std::string& out) {
  if (!noise_manager_) {
    return std::error_code{DaemonErrc::streamer_not_running};
  }
  const noise::DenoiseOutput* dout =
      noise_manager_->get_denoise_output(sink_id);
  if (dout == nullptr || dout->frame_count == 0) {
    return std::error_code{DaemonErrc::streamer_not_running};
  }
  const float* src = denoised ? dout->denoised : dout->noise;
  if (src == nullptr) {
    return std::error_code{DaemonErrc::streamer_not_running};
  }
  float_to_s16_le(src, dout->frame_count, out);
  return std::error_code{};
}
#endif
