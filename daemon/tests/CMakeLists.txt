cmake_minimum_required(VERSION 3.7.0)
project(daemon-test CXX)
set(CMAKE_CXX_STANDARD 17)

# use sysroot cpp-http lib unless one was explicitly provided  in cmdline
if ( NOT CPP_HTTPLIB_DIR )
    find_path( CPP_HTTPLIB_DIR "httplib.h" REQUIRED)
endif()

# ravena lkm _should_ be provided by the CLI. Nonetheless, we should be able
# to find it in system dirs too...
if ( NOT RAVENNNA_ALSA_LKM_DIR )
    find_path(RAVENNA_ALSA_LKM_DIR "common/MergingRAVENNACommon.h" REQUIRED)
endif()

add_definitions( -DBOOST_LOG_DYN_LINK -DBOOST_LOG_USE_NATIVE_SYSLOG )
add_compile_options( -Wall )

set(BOOST_COMPONENTS filesystem thread unit_test_framework)
if(${Boost_VERSION_MINOR} GREATER 87)
    list(APPEND BOOST_COMPONENTS process)
endif()
find_package(Boost COMPONENTS ${BOOST_COMPONENTS})
include_directories(aes67-daemon ${CPP_HTTPLIB_DIR} ${RAVENNA_ALSA_LKM_DIR}/common ${RAVENNA_ALSA_LKM_DIR}/driver ${Boost_INCLUDE_DIR})
add_executable(daemon-test daemon_test.cpp)
target_link_libraries(daemon-test ${Boost_LIBRARIES})
# 注入 daemon 二进制绝对路径（out-of-source 安全）+ 测试源码目录（作 start_dir，
# 使 daemon 的相对路径 daemon.conf / http_base_dir "." / noise_status_file
# "./noise_status.json" 相对 daemon/tests/ 解析）。修复原硬编码 "../aes67-daemon"
# + CWD 相关 "daemon.conf" 在 out-of-source 构建下 execve/配置找不到的问题。
target_compile_definitions(daemon-test PRIVATE
  AES67_DAEMON_BIN="$<TARGET_FILE:aes67-daemon>"
  AES67_DAEMON_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME daemon-test COMMAND daemon-test
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
if(WITH_AVAHI)
  MESSAGE(STATUS "WITH_AVAHI")
  add_definitions(-D_USE_AVAHI_)
endif()
