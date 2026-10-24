cmake_minimum_required(VERSION 3.20)
project(LinkBridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# 目标平台：windows（默认，控制台 + 测试）| android（供 app 的 Gradle/NDK 构建）
set(LB_PLATFORM "windows" CACHE STRING "Target platform: windows | android")

if(WIN32)
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _WINSOCK_DEPRECATED_NO_WARNINGS _WIN32_WINNT=0x0A00)
endif()

# ---- 核心库：core 内所有模块不 include 任何 Socket/系统网络头文件 ----
set(LB_CORE_SOURCES
  src/core/common/bytes.cpp
  src/core/config/toml.cpp
  src/core/config/config.cpp
  src/core/identity/identity.cpp
  src/core/trust/trust_store.cpp
  src/core/pairing/pairing.cpp
  src/core/heartbeat/heartbeat.cpp
  src/core/transfer/transfer.cpp
  src/core/clipboard/clipboard.cpp
  src/core/screen/screen.cpp
  src/core/sync/sync.cpp
  src/core/sync/version_store.cpp
  src/core/node/node.cpp
  # STS 会话与加密分帧（与传输实现解耦，Windows/POSIX 共用）
  src/net/secure_channel.cpp
)

if(LB_PLATFORM STREQUAL "android")
  list(APPEND LB_CORE_SOURCES
    src/pal/android/android_jni.cpp
    src/pal/android/android_paths.cpp
    src/pal/android/android_crypto.cpp
    src/pal/android/android_keystore.cpp
    src/pal/android/android_clipboard.cpp
    src/pal/android/android_screen.cpp
    src/pal/android/android_watcher.cpp
    src/net/tcp_posix.cpp
  )
else()
  list(APPEND LB_CORE_SOURCES
    # 平台适配层（Windows 实现；Linux 复用 tcp_posix + 各自 PAL）
    src/pal/windows/cng_crypto.cpp
    src/pal/windows/dpapi.cpp
    src/pal/windows/netinfo.cpp
    src/pal/windows/win_paths.cpp
    src/pal/windows/win_clipboard.cpp
    src/pal/windows/win_screen.cpp
    src/pal/windows/win_watcher.cpp
    # 传输层（实现 ITransport/ISecureChannel，可被 Mock 替换）
    src/net/tcp.cpp
  )
endif()

add_library(lb_core STATIC ${LB_CORE_SOURCES})
target_include_directories(lb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(WIN32)
  target_link_libraries(lb_core PUBLIC bcrypt crypt32 ws2_32 iphlpapi)
elseif(LB_PLATFORM STREQUAL "android")
  target_link_libraries(lb_core PUBLIC log)
endif()

# Android 只产出核心库，由 android/app 的 NDK 构建再链接 JNI 共享库
if(LB_PLATFORM STREQUAL "android")
  return()
endif()

# ---- 控制台前端 ----
add_executable(linkbridge src/app/main.cpp)
target_link_libraries(linkbridge PRIVATE lb_core)

# ---- 单元测试 ----
enable_testing()
add_executable(lb_tests
  tests/test_main.cpp
  tests/test_crypto.cpp
  tests/test_toml.cpp
  tests/test_pairing.cpp
  tests/test_heartbeat.cpp
  tests/test_transfer.cpp
  tests/test_clipboard.cpp
  tests/test_screen.cpp
  tests/test_sync.cpp
)
target_link_libraries(lb_tests PRIVATE lb_core)
add_test(NAME lb_tests COMMAND lb_tests)

# ---- 端到端测试（双实例 localhost 真实 TCP 配对 + 心跳）----
add_executable(lb_e2e tests/e2e_main.cpp)
target_link_libraries(lb_e2e PRIVATE lb_core)
add_test(NAME lb_e2e COMMAND lb_e2e)
set_tests_properties(lb_e2e PROPERTIES TIMEOUT 60)
