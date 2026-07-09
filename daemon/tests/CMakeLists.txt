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
add_test(daemon-test daemon-test)
if(WITH_AVAHI)
  MESSAGE(STATUS "WITH_AVAHI")
  add_definitions(-D_USE_AVAHI_)
endif()

if(WITH_OCA)
  MESSAGE(STATUS "tests: WITH_OCA")
  # oca-test 初始仅编译 oca_test.cpp。后续每个创建 .cpp 的任务用 target_sources
  # 将其新源加入本目标(CMake 对 add_executable 中不存在的源文件会在配置期报错,
  # 故源文件须随创建随加入)。Task 14 创建 mdns_publisher.cpp 时一并补 AVAHI 条件。
  add_executable(oca-test ${CMAKE_SOURCE_DIR}/oca/tests/oca_test.cpp)
  target_sources(oca-test PRIVATE ${CMAKE_SOURCE_DIR}/oca/ocp1.cpp)
  target_sources(oca-test PRIVATE ${CMAKE_SOURCE_DIR}/oca/session.cpp)
  target_sources(oca-test PRIVATE ${CMAKE_SOURCE_DIR}/oca/classes/root.cpp)
  target_sources(oca-test PRIVATE ${CMAKE_SOURCE_DIR}/oca/classes/device_manager.cpp)
  target_sources(oca-test PRIVATE ${CMAKE_SOURCE_DIR}/oca/classes/network_manager.cpp)
  target_sources(oca-test PRIVATE ${CMAKE_SOURCE_DIR}/oca/classes/subscription_manager.cpp)
  target_sources(oca-test PRIVATE ${CMAKE_SOURCE_DIR}/oca/transport.cpp)
  target_sources(oca-test PRIVATE ${CMAKE_SOURCE_DIR}/oca/oca_server.cpp)
  target_include_directories(oca-test PRIVATE ${CMAKE_SOURCE_DIR})
  target_link_libraries(oca-test ${Boost_LIBRARIES})
  add_test(oca-test oca-test)
endif()
