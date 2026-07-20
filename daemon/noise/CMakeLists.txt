# daemon/noise/CMakeLists.txt
# 噪声模块核心源码（随 spec 渐进追加，Spec1 仅占位空库）。
# 注意：与架构文档 §8.2 的偏差--不使用 $<TARGET_OBJECTS:noise>（仅对 OBJECT
# 库合法），改用 STATIC + 普通链接（daemon/CMakeLists.txt 的 noise-test target
# 用 target_link_libraries(... noise)）。
set(NOISE_SOURCES
  init.cpp
  # Spec1: 暂无其他 .cpp（rcu_ptr.hpp 是 header-only，audio_capture.cpp 在 Task 4 追加）
)

add_library(noise STATIC ${NOISE_SOURCES})
target_include_directories(noise PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
