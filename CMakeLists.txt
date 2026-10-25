cmake_minimum_required(VERSION 3.20)
project(LinkBridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# 目标平台：windows（默认）| linux（POSIX/WSL）| android（供 app 的 Gradle/NDK 构建）
set(LB_PLATFORM "" CACHE STRING "Target platform: windows | linux | android")
if(NOT LB_PLATFORM)
  if(WIN32)
    set(LB_PLATFORM "windows")
  else()
    set(LB_PLATFORM "linux")
  endif()
endif()

if(WIN32)
  # Windows 7 及以上：不声明仅 Win10 可用的 API
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _WINSOCK_DEPRECATED_NO_WARNINGS _WIN32_WINNT=0x0601)
endif()

# Linux 可选 sysroot：无 root 权限时用 `apt-get download` + `dpkg-deb -x` 提取的开发包目录。
# 存在时用它提供 OpenSSL / wxWidgets 的头文件与库；不存在时直接使用系统已安装的开发包。
set(LB_SYSROOT "" CACHE PATH "Linux sysroot 根目录（可选，如 $HOME/lb-sysroot）")
if(NOT WIN32 AND LB_PLATFORM STREQUAL "linux" AND NOT LB_SYSROOT)
  if(EXISTS "$ENV{HOME}/lb-sysroot/usr/include/openssl/evp.h")
    set(LB_SYSROOT "$ENV{HOME}/lb-sysroot")
    message(STATUS "检测到本地 sysroot: ${LB_SYSROOT}")
  endif()
endif()
set(LB_SYSROOT_ARCH_LIB "${LB_SYSROOT}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}")

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
    src/pal/android/android_input.cpp
    src/pal/android/android_watcher.cpp
    src/net/tcp_posix.cpp
  )
elseif(LB_PLATFORM STREQUAL "linux")
  list(APPEND LB_CORE_SOURCES
    # POSIX/Linux 平台适配层（OpenSSL + X11 + inotify）
    src/pal/linux/posix_paths.cpp
    src/pal/linux/openssl_crypto.cpp
    src/pal/linux/secret_store.cpp
    src/pal/linux/x11_clipboard.cpp
    src/pal/linux/x11_screen.cpp
    src/pal/linux/inotify_watcher.cpp
    src/net/tcp_posix.cpp
  )
else()
  list(APPEND LB_CORE_SOURCES
    # 平台适配层（Windows 实现）
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
elseif(LB_PLATFORM STREQUAL "linux")
  if(LB_SYSROOT)
    set(OPENSSL_ROOT_DIR "${LB_SYSROOT}/usr")
    set(OPENSSL_INCLUDE_DIR "${LB_SYSROOT}/usr/include")
    set(OPENSSL_CRYPTO_LIBRARY "${LB_SYSROOT_ARCH_LIB}/libcrypto.so")
    # 多架构头文件目录（opensslconf.h 等按架构生成）
    set(LB_SYSROOT_ARCH_INCLUDE "${LB_SYSROOT}/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}")
  endif()
  find_package(OpenSSL REQUIRED)
  find_package(Threads REQUIRED)
  find_package(X11 REQUIRED)
  target_link_libraries(lb_core PUBLIC OpenSSL::Crypto Threads::Threads X11::X11)
  if(LB_SYSROOT_ARCH_INCLUDE)
    target_include_directories(lb_core SYSTEM PUBLIC "${LB_SYSROOT_ARCH_INCLUDE}")
  endif()
endif()

# Android 只产出核心库，由 android/app 的 NDK 构建再链接 JNI 共享库
if(LB_PLATFORM STREQUAL "android")
  return()
endif()

# ---- 控制台前端 ----
add_executable(linkbridge src/app/main.cpp)
target_link_libraries(linkbridge PRIVATE lb_core)

# ---- 图形界面（wxWidgets 3.2：Win32 / GTK 原生控件）----
option(LB_BUILD_GUI "Build the wxWidgets GUI frontend" ON)
set(LB_WX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/wxWidgets-3.2.6" CACHE PATH "wxWidgets 源码目录")
set(LB_WX_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/wx-build" CACHE PATH "wxWidgets 预编译产物目录")

if(LB_BUILD_GUI)
  if(WIN32)
    if(NOT EXISTS "${LB_WX_SOURCE_DIR}/include/wx/wx.h")
      message(WARNING "未找到 wxWidgets 源码，跳过 GUI（可设置 LB_WX_SOURCE_DIR）")
      set(LB_BUILD_GUI OFF)
    elseif(NOT EXISTS "${LB_WX_BUILD_DIR}")
      message(WARNING "未找到 wxWidgets 预编译目录，跳过 GUI（可设置 LB_WX_BUILD_DIR）")
      set(LB_BUILD_GUI OFF)
    endif()
  else()
    # Linux：wxWidgets 3.2 GTK3 共享库（发行版包 libwxgtk3.2-dev / wx3.2-headers）
    set(LB_WX_GTK_ROOT "" CACHE PATH "wxWidgets 3.2 (GTK3) 安装根目录")
    if(NOT LB_WX_GTK_ROOT)
      if(LB_SYSROOT AND EXISTS "${LB_SYSROOT}/usr/include/wx-3.2/wx/wx.h")
        set(LB_WX_GTK_ROOT "${LB_SYSROOT}/usr")
      elseif(EXISTS "/usr/include/wx-3.2/wx/wx.h")
        set(LB_WX_GTK_ROOT "/usr")
      endif()
    endif()
    if(NOT LB_WX_GTK_ROOT)
      message(WARNING "未找到 wxWidgets 3.2（GTK3），跳过 GUI（可设置 LB_WX_GTK_ROOT）")
      set(LB_BUILD_GUI OFF)
    endif()
  endif()
endif()

if(LB_BUILD_GUI)
  add_library(lb_wx INTERFACE)
  if(WIN32)
    target_include_directories(lb_wx INTERFACE "${LB_WX_SOURCE_DIR}/include")
    set(LB_WX_LIB_DIR "${LB_WX_BUILD_DIR}/lib/gcc_x64_lib")
    target_include_directories(lb_wx INTERFACE "${LB_WX_LIB_DIR}/mswu")
    set(LB_WX_LIBS "")
    foreach(_wxname wxmsw32u_html wxmsw32u_core wxbase32u wxregexu wxpng wxjpeg wxtiff wxzlib wxexpat)
      list(APPEND LB_WX_LIBS "${LB_WX_LIB_DIR}/lib${_wxname}.a")
    endforeach()
    target_link_libraries(lb_wx INTERFACE ${LB_WX_LIBS}
      comctl32 comdlg32 gdi32 ole32 oleaut32 uuid rpcrt4 shell32 shlwapi
      user32 winspool advapi32 winmm gdiplus msimg32 uxtheme oleacc version)
  else()
    # 头文件：wx-3.2 通用头 + 该构建配置的 wx/setup.h
    target_include_directories(lb_wx INTERFACE
      "${LB_WX_GTK_ROOT}/include/wx-3.2"
      "${LB_WX_GTK_ROOT}/lib/${CMAKE_LIBRARY_ARCHITECTURE}/wx/include/gtk3-unicode-3.2")
    target_compile_definitions(lb_wx INTERFACE __WXGTK__)
    set(LB_WX_GTK_LIB_DIR "${LB_WX_GTK_ROOT}/lib/${CMAKE_LIBRARY_ARCHITECTURE}")
    target_link_directories(lb_wx INTERFACE "${LB_WX_GTK_LIB_DIR}")
    # wx 依赖的 GTK3/音频等运行时库可能只在 sysroot 中（无 root 环境）
    target_link_options(lb_wx INTERFACE
      "-Wl,-rpath-link,${LB_WX_GTK_LIB_DIR}"
      "-Wl,-rpath-link,${LB_WX_GTK_LIB_DIR}/pulseaudio")
    target_link_libraries(lb_wx INTERFACE
      wx_gtk3u_html-3.2 wx_gtk3u_core-3.2 wx_baseu-3.2
      Threads::Threads ${CMAKE_DL_LIBS})
  endif()

  add_executable(linkbridge-gui
    src/gui/gui_app.cpp
    src/gui/gui_controller.cpp
    src/gui/gui_frame.cpp
    src/gui/gui_panels_basic.cpp
    src/gui/gui_panels_tools.cpp
    src/gui/gui_panels_sync.cpp
  )
  target_link_libraries(linkbridge-gui PRIVATE lb_core lb_wx)
  if(WIN32)
    enable_language(RC)
    target_sources(linkbridge-gui PRIVATE src/gui/linkbridge.rc)
    target_link_libraries(linkbridge-gui PRIVATE psapi)
    set_target_properties(linkbridge-gui PROPERTIES WIN32_EXECUTABLE TRUE)
  else()
    # 共享库版 wx 需要运行时能找到 libwx_gtk3u_*.so（sysroot 场景）
    set_target_properties(linkbridge-gui PROPERTIES
      BUILD_RPATH "${LB_WX_GTK_LIB_DIR};${LB_WX_GTK_LIB_DIR}/pulseaudio"
      INSTALL_RPATH "${LB_WX_GTK_LIB_DIR};${LB_WX_GTK_LIB_DIR}/pulseaudio")
  endif()
endif()

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
