# daemon/noise/CMakeLists.txt
# 噪声模块核心源码（随 spec 渐进追加，Spec1 仅占位空库）。
# 注意：与架构文档 §8.2 的偏差--不使用 $<TARGET_OBJECTS:noise>（仅对 OBJECT
# 库合法），改用 STATIC + 普通链接（daemon/CMakeLists.txt 的 noise-test target
# 用 target_link_libraries(... noise)）。
set(NOISE_SOURCES
  audio_capture.cpp
  noise_manager.cpp
  noise_metrics.cpp
  noise_http.cpp
  noise_status.cpp
  vad.cpp
  noise_detector.cpp
  denoise_plugin_factory.cpp
  denoise_processor.cpp
  model-adapters/passthrough_plugin.cpp
  noise_analyzer.cpp
  noise_template_db.cpp
  ref_comparator.cpp
  sse_broadcaster.cpp
  resampler.cpp
  ml_classifier.cpp
)

add_library(noise STATIC ${NOISE_SOURCES})
target_include_directories(noise PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Spec5 T1：SpeexDSP 重采样依赖（native↔48k，resampler.cpp）。
# 本文件仅在 daemon/CMakeLists.txt 的 if(WITH_NOISE) 内 add_subdirectory，
# 故 WITH_NOISE=OFF 时此块不执行，不引入 SpeexDSP（--no-noise 零回归）。
# libspeexdsp 无 CMake config（仅 .pc），用 find_path/find_library（与 daemon
# 既有 ALSA/AVAHI/faac 同模式；不用 pkg_check_modules 以匹配本仓库风格）。
# resampler.hpp 是 noise 的 PUBLIC 头（noise_manager.hpp 经它暴露 SpeexDSP
# 类型），故 include + link 用 PUBLIC：noise-test / aes67-daemon 经 noise
# 传递性获得 SpeexDSP 头与库。
find_path(SPEEXDSP_INCLUDE_DIR speex/speex_resampler.h)
find_library(SPEEXDSP_LIBRARY NAMES speexdsp)
if(NOT SPEEXDSP_INCLUDE_DIR OR NOT SPEEXDSP_LIBRARY)
  message(FATAL_ERROR
    "SpeexDSP not found (install libspeexdsp-dev, e.g. debian-packages.sh)")
endif()
target_include_directories(noise PUBLIC ${SPEEXDSP_INCLUDE_DIR})
target_link_libraries(noise PUBLIC ${SPEEXDSP_LIBRARY})

# RNNoise 降噪插件（Spec2 1.6b）。
# 整个 daemon/noise/CMakeLists.txt 由 daemon/CMakeLists.txt 的
# add_subdirectory(noise) 在 if(WITH_NOISE) 内引入,故 WITH_NOISE=OFF
# 时本文件不执行,submodule 不会被触发（--no-noise 零回归保证）。
#
# RNNoise build-system 偏差（详见 task-4-report.md）：
# - task brief 指定 pinned commit 70f1d25。该 commit 的 rnnoise_data.{c,h}
#   不在 git 仓库--需从 media.xiph.org 下载 58MB model tarball。
# - RNNoise 无 CMakeLists.txt（纯 autotools）,故手动 add_library(rnnoise
#   STATIC ...) 编译 .c 源码（绕过 autotools）。
# - 首次配置时自动下载 model tarball（有 SHA256 校验）,缓存在 build 目录下
#   避免重复下载。后续配置检测到 src/rnnoise_data.c 存在则跳过下载。
# - Spec3 T1：源码来源由 FetchContent_Populate 改为 vendored submodule
#   3rdparty/rnnoise（pinned 70f1d25）。CMake 3.30+ 已弃用
#   FetchContent_Populate；submodule 方式更简单且可重复。
option(NOISE_PLUGIN_RNNOISE "Build RNNoise denoise plugin" ON)
if(NOISE_PLUGIN_RNNOISE)
  # RNNoise 是纯 C 库,而 daemon/CMakeLists.txt 的 project() 仅启用 CXX。
  # 在本子目录内 enable_language(C) 启用 C 编译器（不修改 daemon/CMakeLists.txt）。
  enable_language(C)

  # 源码目录：vendored submodule（3rdparty/rnnoise，pinned 70f1d25）。
  # build.sh/buildfake.sh 的 `git submodule update --init --recursive` 负责 init。
  # 若 gitlab.xiph.org 不可达,改用 github 镜像重新 add：
  #   git submodule add https://github.com/xiph/rnnoise.git 3rdparty/rnnoise
  set(RNNOISE_SRC_DIR ${CMAKE_SOURCE_DIR}/../3rdparty/rnnoise)

  # rnnoise_data.{c,h} 不在 git 仓库--由 download_model.sh 从
  # media.xiph.org 下载 model tarball 解压。CMake 首次配置时自动下载
  # （~58MB,有 SHA256 校验）。缓存在 build 目录避免重复下载。
  # 后续配置检测到文件存在则跳过。
  file(STRINGS "${RNNOISE_SRC_DIR}/model_version" RNNOISE_MODEL_HASH)
  set(RNNOISE_MODEL_URL
      "https://media.xiph.org/rnnoise/models/rnnoise_data-${RNNOISE_MODEL_HASH}.tar.gz")
  set(RNNOISE_MODEL_FILE
      "${CMAKE_BINARY_DIR}/rnnoise-model-cache/rnnoise_data-${RNNOISE_MODEL_HASH}.tar.gz")
  if(NOT EXISTS "${RNNOISE_SRC_DIR}/src/rnnoise_data.c")
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
      WORKING_DIRECTORY ${RNNOISE_SRC_DIR}
      RESULT_VARIABLE tar_result)
    if(NOT tar_result EQUAL 0)
      message(FATAL_ERROR "Failed to extract RNNoise model tarball")
    endif()
  endif()

  # 编译 RNNoise C 源码为静态库（绕过 autotools）。
  # 源码清单来自 RNNoise Makefile.am 的 RNNOISE_SOURCES（不含 x86 RTCD,
  # 默认 portable C 实现;若需 SIMD 优化可后续扩展）。
  set(RNNOISE_C_SOURCES
    ${RNNOISE_SRC_DIR}/src/denoise.c
    ${RNNOISE_SRC_DIR}/src/rnn.c
    ${RNNOISE_SRC_DIR}/src/pitch.c
    ${RNNOISE_SRC_DIR}/src/kiss_fft.c
    ${RNNOISE_SRC_DIR}/src/celt_lpc.c
    ${RNNOISE_SRC_DIR}/src/nnet.c
    ${RNNOISE_SRC_DIR}/src/nnet_default.c
    ${RNNOISE_SRC_DIR}/src/parse_lpcnet_weights.c
    ${RNNOISE_SRC_DIR}/src/rnnoise_data.c
    ${RNNOISE_SRC_DIR}/src/rnnoise_tables.c
  )
  add_library(rnnoise STATIC ${RNNOISE_C_SOURCES})
  # include/ 暴露 rnnoise.h（PUBLIC,使用方 #include "rnnoise.h" 可见）;
  # src/ 是内部头文件（arch.h/common.h 等,PRIVATE）。
  target_include_directories(rnnoise PUBLIC ${RNNOISE_SRC_DIR}/include)
  target_include_directories(rnnoise PRIVATE ${RNNOISE_SRC_DIR}/src)
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

# ── Spec5 T2：ONNX Runtime 统一后端 + DTLN/DeepFilterNet 插件 ──────────────
# denoise-plugin §0.1/§3.2/§3.3：DTLN/DFN 经 ONNX Runtime C++ 加载 ONNX 模型。
# onnxruntime 无系统包（venv 仅 Python binding），下载官方 C/C++ release 到
# build 缓存目录（与 RNNoise model tarball 同模式：file(DOWNLOAD) + 缓存）。
# 版本 1.27.0（与验证环境 venv onnxruntime 一致）。
option(NOISE_PLUGIN_DTLN "Build DTLN denoise plugin (needs ONNX Runtime)" ON)
option(NOISE_PLUGIN_DEEPFILTER "Build DeepFilterNet denoise plugin (needs ONNX Runtime)" ON)

if(NOISE_PLUGIN_DTLN OR NOISE_PLUGIN_DEEPFILTER)
  set(ORT_VERSION "1.27.0")
  set(ORT_TGZ_NAME "onnxruntime-linux-x64-${ORT_VERSION}.tgz")
  set(ORT_CACHE_DIR "${CMAKE_BINARY_DIR}/onnxruntime-cache")
  set(ORT_TGZ "${ORT_CACHE_DIR}/${ORT_TGZ_NAME}")
  set(ORT_EXTRACT_DIR "${ORT_CACHE_DIR}/onnxruntime-linux-x64-${ORT_VERSION}")
  if(NOT EXISTS "${ORT_EXTRACT_DIR}/include/onnxruntime_cxx_api.h")
    if(EXISTS "${ORT_TGZ}")
      message(STATUS "Using cached ONNX Runtime tarball")
    else()
      message(STATUS "Downloading ONNX Runtime C/C++ release (~30MB)...")
      file(DOWNLOAD
        "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_TGZ_NAME}"
        ${ORT_TGZ} SHOW_PROGRESS TIMEOUT 600)
    endif()
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E tar xf ${ORT_TGZ}
      WORKING_DIRECTORY ${ORT_CACHE_DIR}
      RESULT_VARIABLE ort_result)
    if(NOT ort_result EQUAL 0)
      message(FATAL_ERROR "Failed to extract ONNX Runtime tarball")
    endif()
  endif()

  # find_path/find_library 指向解压目录（不污染系统搜索路径）。
  find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
            PATHS "${ORT_EXTRACT_DIR}/include" NO_DEFAULT_PATH)
  # release 同时提供 libonnxruntime.so(.1/.1.27.0) 三个符号链接/文件。
  find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime onnxruntime.1
               PATHS "${ORT_EXTRACT_DIR}/lib" NO_DEFAULT_PATH)
  if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
    message(FATAL_ERROR "ONNX Runtime headers/lib not found in ${ORT_EXTRACT_DIR}")
  endif()
  message(STATUS "ONNX Runtime: ${ONNXRUNTIME_LIBRARY}")
  # ONNX 基建 TU（Ort::Env 单例 + CreateOnnxSession）：仅 onnxruntime 可用时编译
  # （onnx_session.hpp include onnxruntime_cxx_api.h，无 ORT 时不可编译）。
  target_sources(noise PRIVATE onnx_session.cpp)
  # noise 的 PUBLIC 头（onnx_session.hpp）include onnxruntime_cxx_api.h，故
  # include 用 PUBLIC：noise-test / aes67-daemon 经 noise 传递性获得头。
  target_include_directories(noise PUBLIC ${ONNXRUNTIME_INCLUDE_DIR})
  # 链接 full-path .so：CMake 默认把 lib 所在目录加入可执行文件 RPATH，
  # 使 noise-test / aes67-daemon 运行时能找到 libonnxruntime.so。
  target_link_libraries(noise PUBLIC ${ONNXRUNTIME_LIBRARY})
  # 显式 BUILD_RPATH 兜底（部分 CMake 版本对 PRIVATE 传递不自动加 RPATH）。
  set(ONNXRUNTIME_LIB_DIR "${ORT_EXTRACT_DIR}/lib" CACHE PATH "ONNX Runtime lib dir")
endif()

if(NOISE_PLUGIN_DTLN)
  target_sources(noise PRIVATE model-adapters/dtln/dtln_adapter.cpp)
endif()
if(NOISE_PLUGIN_DEEPFILTER)
  target_sources(noise PRIVATE
    model-adapters/deepfilternet/deepfilternet_adapter.cpp)
endif()
