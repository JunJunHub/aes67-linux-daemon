//
//  main.cpp
//
//  Copyright (c) 2019 2025 Andrea Bondavalli. All rights reserved.
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
//

#include <boost/program_options.hpp>
#include <iostream>
#include <thread>

#include "browser.hpp"
#include "config.hpp"
#include "driver_interface.hpp"
#include "http_server.hpp"
#include "interface.hpp"
#include "log.hpp"
#include "mdns_server.hpp"
#include "rtsp_server.hpp"
#include "session_manager.hpp"

#ifdef _USE_STREAMER_
#include "streamer.hpp"
#endif

#ifdef _USE_NOISE_
#include "noise/noise_http.hpp"
#include "noise/noise_manager.hpp"
#include "noise/noise_template_db.hpp"
#include "noise_session_manager_bridge.hpp"
#include "pcm_capture_service.hpp"
#endif

#ifdef _USE_SYSTEMD_
#include <systemd/sd-daemon.h>
#endif

namespace po = boost::program_options;
namespace postyle = boost::program_options::command_line_style;
namespace logging = boost::log;

static const std::string version("bondagit-3.1.0");
static std::atomic<bool> terminate = false;

void termination_handler(int signum) {
  BOOST_LOG_TRIVIAL(info) << "main:: got signal " << signum;
  // Terminate program
  terminate = true;
}

bool is_terminated() {
  return terminate.load();
}

const std::string& get_version() {
  return version;
}

int main(int argc, char* argv[]) {
  int rc(EXIT_SUCCESS);
  po::options_description desc("Options");
  desc.add_options()("version,v", "Print daemon version and exit")(
      "config,c", po::value<std::string>()->default_value("/etc/daemon.conf"),
      "daemon configuration file")("http_addr,a", po::value<std::string>(),
                                   "HTTP server addr")(
      "http_port,p", po::value<int>(), "HTTP server port")("help,h",
                                                           "Print this help "
                                                           "message");
  int unix_style = postyle::unix_style | postyle::short_allow_next;
  bool driver_restart(true);

#ifdef _USE_SYSTEMD_
  // with which interval we should pet the dog
  uint64_t current_watchdog_usec;
#endif

  po::variables_map vm;
  try {
    po::store(po::command_line_parser(argc, argv)
                  .options(desc)
                  .style(unix_style)
                  .run(),
              vm);

    po::notify(vm);

    if (vm.count("version")) {
      std::cout << version << '\n';
      return EXIT_SUCCESS;
    }
    if (vm.count("help")) {
      std::cout << "USAGE: " << argv[0] << '\n' << desc << '\n';
      return EXIT_SUCCESS;
    }

  } catch (po::error& poe) {
    std::cerr << poe.what() << '\n'
              << "USAGE: " << argv[0] << '\n'
              << desc << '\n';
    return EXIT_FAILURE;
  }

  signal(SIGINT, termination_handler);
  signal(SIGTERM, termination_handler);
  signal(SIGCHLD, SIG_IGN);

  std::srand(std::time(nullptr));

  std::string filename = vm["config"].as<std::string>();

#ifdef _USE_SYSTEMD_
  sd_watchdog_enabled(0, &current_watchdog_usec);

  if (current_watchdog_usec > 0) {
    // Inform systemd that if we're not petting the dog in 5s we're bust.
    sd_notify(0, "WATCHDOG_USEC=10000000");

    current_watchdog_usec = 10000000;
  }
#endif

  while (!is_terminated() && rc == EXIT_SUCCESS) {
    /* load configuration from file */
    auto config = Config::parse(filename, driver_restart);
    if (config == nullptr) {
      return EXIT_FAILURE;
    }

    if (vm.count("http_addr")) {
      config->set_http_addr_str(vm["http_addr"].as<std::string>());
    }
    /* override configuration according to command line args */
    if (vm.count("http_port")) {
      config->set_http_port(vm["http_port"].as<int>());
    }
    /* init logging */
    log_init(*config);

    if (config->get_ip_addr_str().empty()) {
#ifdef _USE_SYSTEMD_
      if (current_watchdog_usec > 0)
        sd_notify(0, "WATCHDOG=1");
      sd_notify(0, "STATUS=no IP address, waiting ...");
#endif
      BOOST_LOG_TRIVIAL(info) << "main:: no IP address, waiting ...";
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }

    BOOST_LOG_TRIVIAL(debug) << "main:: initializing daemon";
    try {
      auto driver = DriverManager::create();
      /* setup and init driver */
      if (driver == nullptr || !driver->init(*config)) {
        throw std::runtime_error(std::string("DriverManager:: init failed"));
      }

      /* start browser */
      auto browser = Browser::create(config);
      if (browser == nullptr || !browser->init()) {
        throw std::runtime_error(std::string("Browser:: init failed"));
      }

      /* start session manager */
      auto session_manager = SessionManager::create(driver, browser, config);
      if (session_manager == nullptr || !session_manager->init()) {
        throw std::runtime_error(std::string("SessionManager:: init failed"));
      }

#ifdef _USE_NOISE_
      // Spec3 Task 6 1.11 装配（arch §10 1.11 + §4.4）：
      //   PcmCaptureService::create(session_manager, config)
      //   -> NoiseSessionManagerBridge(pcm_capture)
      //   -> NoiseManager(bridge)
      //   -> load_status (sensor 配置) + template_db.load (模板库)
      //   -> register_noise_routes (sensor + template HTTP 路由)
      //   -> pcm_capture->set_capture_joined_callback (T7 path A：PTP unlock
      //      后 capture 线程 join -> NoiseManager::on_capture_thread_joined)
      //   -> streamer->set_noise_manager (Streamer 三路 AAC 持引用)
      // 顺序约束：必须在 session_manager 之后（PcmCaptureService 注册其
      // observer），在 streamer/http_server 之前（streamer 需注入
      // noise_manager， http_server 需注入 pcm_capture 用于 /denoised /noise
      // 路由）。
      auto pcm_capture = PcmCaptureService::create(session_manager, config);
      if (pcm_capture == nullptr || !pcm_capture->init()) {
        throw std::runtime_error(
            std::string("PcmCaptureService:: init failed"));
      }
      auto noise_bridge =
          std::make_shared<NoiseSessionManagerBridge>(pcm_capture);
      auto noise_manager = std::make_shared<noise::NoiseManager>(*noise_bridge);
      // 持久化加载（arch §7.6 / §7.5）。文件不存在视为首次启动（no-op）。
      if (!config->get_noise_status_file().empty()) {
        noise_manager->load_status(config->get_noise_status_file());
      }
      auto noise_template_db = std::make_shared<noise::NoiseTemplateDB>();
      if (!config->get_noise_template_dir().empty()) {
        noise_template_db->load(config->get_noise_template_dir());
      }
      // T7 path A 回调：PcmCaptureService 在 PTP unlock -> stop_capture join
      // capture 线程后回调 -> NoiseManager::on_capture_thread_joined（控制
      // 线程，capture 已静止 -> 安全 plugin->reset()）。
      pcm_capture->set_capture_joined_callback(
          [noise_manager]() { noise_manager->on_capture_thread_joined(); });
#endif

      /* start mDNS server */
      MDNSServer mdns_server(session_manager, config);
      if (config->get_mdns_enabled() && !mdns_server.init()) {
        throw std::runtime_error(std::string("MDNSServer:: init failed"));
      }

      /* start rtsp server */
      RtspServer rtsp_server(session_manager, config);
      if (!rtsp_server.init()) {
        throw std::runtime_error(std::string("RtspServer:: init failed"));
      }

      /* start streamer */
#ifdef _USE_STREAMER_
      auto streamer = Streamer::create(session_manager, config);
      if (config->get_streamer_enabled() &&
          (streamer == nullptr || !streamer->init())) {
        throw std::runtime_error(std::string("Streamer:: init failed"));
      }

      /* start http server */
      HttpServer http_server(session_manager, browser, streamer, config);
#else
      HttpServer http_server(session_manager, browser, config);
#endif
      if (!http_server.init()) {
        throw std::runtime_error(std::string("HttpServer:: init failed"));
      }

#ifdef _USE_NOISE_
      // Spec3 Task 6：Streamer 持 noise_manager 引用（三路 AAC /denoised /noise
      // 路由需要，arch §4.4）。WITH_STREAMER=OFF 时 streamer 不存在，跳过
      // （Phase 1 限定：denoise/noise AAC 路由仅在 WITH_STREAMER+WITH_NOISE
      // 同时启用时可用）。
#ifdef _USE_STREAMER_
      if (streamer)
        streamer->set_noise_manager(noise_manager);
#endif
      // 注册 /api/noise/* 路由（sensor + template，arch §5.1/§5.3）。
      // http_server.init() 之后调用（svr_ 已配置，未 listen）。
      noise::register_noise_routes(http_server.server(), *noise_manager,
                                   *noise_template_db);
#endif

      /* load session status from file */
      session_manager->load_status();

      BOOST_LOG_TRIVIAL(debug) << "main:: init done, entering loop...";

#ifdef _USE_SYSTEMD_
      // To be able to use sd_notify at all have to set service NotifyAccess
      // (e.g. to main)
      sd_notify(0, "READY=1");  // If service Type=notify the service is only
                                // considered ready once we send this (this is
                                // independent of watchdog capability)
      sd_notify(0, "STATUS=Working");
#endif

      while (!is_terminated()) {
#ifdef _USE_SYSTEMD_
        if (current_watchdog_usec > 0)
          sd_notify(0, "WATCHDOG=1");
#endif

        auto [ip_addr, ip_str, is_new] = get_new_interface_ip(
            config->get_interface_name(0), config->get_ip_addr_str());
        if (is_new) {
          BOOST_LOG_TRIVIAL(warning)
              << "main:: IP address changed, restarting ...";
          break;
        }

        driver_restart = config->get_driver_restart();
        if (config->get_daemon_restart()) {
          BOOST_LOG_TRIVIAL(warning) << "main:: config changed, restarting ...";
          break;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
#ifdef _USE_SYSTEMD_
      if (is_terminated()) {
        sd_notify(0, "STOPPING=1");
        sd_notify(0, "STATUS=Stopping");
      } else {
        sd_notify(0, "RELOADING=1");
        sd_notify(0, "STATUS=Restarting");
      }
#endif

      /* save session status to file */
      session_manager->save_status();
#ifdef _USE_NOISE_
      // Spec3 Task 6：shutdown 序列保存 noise 传感器配置（arch §7.6）。
      // 持 ctrl_mutex_ 防与并发 HTTP 控制操作竞态（review Minor #2）。
      if (noise_manager)
        noise_manager->save_status_on_exit();
      if (noise_template_db && !config->get_noise_template_dir().empty())
        noise_template_db->save(config->get_noise_template_dir());
#endif

      /* stop http server */
      if (!http_server.terminate()) {
        throw std::runtime_error(std::string("HttpServer:: terminate failed"));
      }

      /* stop streamer */
#ifdef _USE_STREAMER_
      if (config->get_streamer_enabled()) {
        if (!streamer->terminate()) {
          throw std::runtime_error(std::string("Streamer:: terminate failed"));
        }
      }
#endif

      /* stop rtsp server */
      if (!rtsp_server.terminate()) {
        throw std::runtime_error(std::string("RtspServer:: terminate failed"));
      }

      /* stop mDNS server */
      if (config->get_mdns_enabled()) {
        if (!mdns_server.terminate()) {
          throw std::runtime_error(std::string("MDNServer:: terminate failed"));
        }
      }

      /* stop session manager */
      if (!session_manager->terminate()) {
        throw std::runtime_error(
            std::string("SessionManager:: terminate failed"));
      }

      /* stop browser */
      if (!browser->terminate()) {
        throw std::runtime_error(std::string("Browser:: terminate failed"));
      }

      /* stop driver manager */
      if (!driver->terminate(*config)) {
        throw std::runtime_error(
            std::string("DriverManager:: terminate failed"));
      }

    } catch (std::exception& e) {
      BOOST_LOG_TRIVIAL(fatal) << "main:: fatal exception error: " << e.what();
      rc = EXIT_FAILURE;
    }

    BOOST_LOG_TRIVIAL(info) << "main:: end ";
  }

  std::cout << "daemon exiting with code: " << rc << std::endl;
  return rc;
}
