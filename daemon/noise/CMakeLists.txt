# daemon/noise/CMakeLists.txt
# 噪声模块核心源码（随 spec 渐进追加，Spec1 仅占位空库）。
# 注意：与架构文档 §8.2 的偏差--不使用 $<TARGET_OBJECTS:noise>（仅对 OBJECT
# 库合法），改用 STATIC + 普通链接（daemon/CMakeLists.txt 的 noise-test target
# 用 target_link_libraries(... noise)）。
set(NOISE_SOURCES
  audio_capture.cpp
  noise_manager.cpp
  vad.cpp
  noise_detector.cpp
  denoise_plugin_factory.cpp
  denoise_processor.cpp
  model-adapters/passthrough_plugin.cpp
  noise_analyzer.cpp
  noise_template_db.cpp
)

add_library(noise STATIC ${NOISE_SOURCES})
target_include_directories(noise PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# RNNoise 降噪插件（Spec2 1.6b）。
# 整个 daemon/noise/CMakeLists.txt 由 daemon/CMakeLists.txt 的
# add_subdirectory(noise) 在 if(WITH_NOISE) 内引入,故 WITH_NOISE=OFF
# 时本文件不执行,FetchContent 不会被触发（--no-noise 零回归保证）。
#
# RNNoise build-system 偏差（详见 task-4-report.md）：
# - task brief 指定 pinned commit 70f1d25。该 commit 的 rnnoise_data.{c,h}
#   不在 git 仓库--需从 media.xiph.org 下载 58MB model tarball。
# - RNNoise 无 CMakeLists.txt（纯 autotools）,故 FetchContent_MakeAvailable
#   会静默跳过 add_subdirectory,导致 target_link_libraries(noise PRIVATE rnnoise)
#   误解析为系统 -lrnnoise。改为 FetchContent_Populate 仅下载源码,
#   手动 add_library(rnnoise STATIC ...) 编译 .c 源码。
# - 首次配置时自动下载 model tarball（有 SHA256 校验）,缓存在 build 目录下
#   避免重复下载。后续配置检测到 src/rnnoise_data.c 存在则跳过下载。
include(FetchContent)
option(NOISE_PLUGIN_RNNOISE "Build RNNoise denoise plugin" ON)
if(NOISE_PLUGIN_RNNOISE)
  # RNNoise 是纯 C 库,而 daemon/CMakeLists.txt 的 project() 仅启用 CXX。
  # 在本子目录内 enable_language(C) 启用 C 编译器（不修改 daemon/CMakeLists.txt）。
  enable_language(C)

  # GIT_TAG 用 pinned commit 而非 master：master 会漂移,可能破坏构建。
  # 网络已预验证：gitlab.xiph.org HEAD = 70f1d256... (2026-07)。
  # 若 gitlab 不可达,改用 github 镜像：https://github.com/xiph/rnnoise.git。
  FetchContent_Declare(rnnoise
    GIT_REPOSITORY https://gitlab.xiph.org/xiph/rnnoise.git
    GIT_TAG 70f1d256acd4b34a572f999a05c87bf00b67730d)
  FetchContent_GetProperties(rnnoise)
  if(NOT rnnoise_POPULATED)
    FetchContent_Populate(rnnoise)
  endif()

  # rnnoise_data.{c,h} 不在 git 仓库--由 download_model.sh 从
  # media.xiph.org 下载 model tarball 解压。CMake 首次配置时自动下载
  # （~58MB,有 SHA256 校验）。缓存在 build 目录避免重复下载。
  # 后续配置检测到文件存在则跳过。
  file(STRINGS "${rnnoise_SOURCE_DIR}/model_version" RNNOISE_MODEL_HASH)
  set(RNNOISE_MODEL_URL
      "https://media.xiph.org/rnnoise/models/rnnoise_data-${RNNOISE_MODEL_HASH}.tar.gz")
  set(RNNOISE_MODEL_FILE
      "${CMAKE_BINARY_DIR}/rnnoise-model-cache/rnnoise_data-${RNNOISE_MODEL_HASH}.tar.gz")
  if(NOT EXISTS "${rnnoise_SOURCE_DIR}/src/rnnoise_data.c")
    if(EXISTS ${RNNOISE_MODEL_FILE})
      message(STATUS "Using cached RNNoise model tarball")
    else()
      message(STATUS "Downloading RNNoise model data (~58MB)...")
      file(DOWNLOAD ${RNNOISE_MODEL_URL} ${RNNOISE_MODEL_FILE}
           SHOW_PROGRESS
           EXPECTED_HASH SHA256=${RNNOISE_MODEL_HASH}
           TIMEOUT 600)
    endif()
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xf ${RNNOISE_MODEL_FILE}
      WORKING_DIRECTORY ${rnnoise_SOURCE_DIR}
      RESULT_VARIABLE tar_result)
    if(NOT tar_result EQUAL 0)
      message(FATAL_ERROR "Failed to extract RNNoise model tarball")
    endif()
  endif()

  # 编译 RNNoise C 源码为静态库（绕过 autotools）。
  # 源码清单来自 RNNoise Makefile.am 的 RNNOISE_SOURCES（不含 x86 RTCD,
  # 默认 portable C 实现;若需 SIMD 优化可后续扩展）。
  set(RNNOISE_C_SOURCES
    ${rnnoise_SOURCE_DIR}/src/denoise.c
    ${rnnoise_SOURCE_DIR}/src/rnn.c
    ${rnnoise_SOURCE_DIR}/src/pitch.c
    ${rnnoise_SOURCE_DIR}/src/kiss_fft.c
    ${rnnoise_SOURCE_DIR}/src/celt_lpc.c
    ${rnnoise_SOURCE_DIR}/src/nnet.c
    ${rnnoise_SOURCE_DIR}/src/nnet_default.c
    ${rnnoise_SOURCE_DIR}/src/parse_lpcnet_weights.c
    ${rnnoise_SOURCE_DIR}/src/rnnoise_data.c
    ${rnnoise_SOURCE_DIR}/src/rnnoise_tables.c
  )
  add_library(rnnoise STATIC ${RNNOISE_C_SOURCES})
  # include/ 暴露 rnnoise.h（PUBLIC,使用方 #include "rnnoise.h" 可见）;
  # src/ 是内部头文件（arch.h/common.h 等,PRIVATE）。
  target_include_directories(rnnoise PUBLIC ${rnnoise_SOURCE_DIR}/include)
  target_include_directories(rnnoise PRIVATE ${rnnoise_SOURCE_DIR}/src)
  set_target_properties(rnnoise PROPERTIES
                        POSITION_INDEPENDENT_CODE ON
                        C_STANDARD 99)
  # RNNoise 仅依赖 libm（lrintf 等）。
  find_library(M_LIB m)
  if(M_LIB)
    target_link_libraries(rnnoise PUBLIC ${M_LIB})
  endif()

  target_sources(noise PRIVATE model-adapters/rnnoise/rnnoise_adapter.cpp)
  # rnnoise 是已定义的 CMake target（不是系统库）,链接即暴露 PUBLIC include。
  # 用 PUBLIC 而非 PRIVATE：rnnoise_adapter.hpp（noise 的 PUBLIC 头）include
  # "rnnoise.h",故 noise 的消费者（noise-test / aes67-daemon）需可见 rnnoise 的
  # include 目录才能编译包含 adapter 头的源文件。
  target_link_libraries(noise PUBLIC rnnoise)
endif()
