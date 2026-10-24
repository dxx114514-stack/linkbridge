# Android NDK 入口：只构建核心静态库 lb_core，再链接 JNI 共享库。
# 由 app/build.gradle.kts 的 externalNativeBuild 调用。
cmake_minimum_required(VERSION 3.22)

project(lbcore_jni LANGUAGES CXX)

set(LB_PLATFORM "android" CACHE STRING "Target platform" FORCE)

# 根工程：core + Android PAL + POSIX socket，只产出 lb_core（Windows 前端自动跳过）
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. lbcore)

add_library(lbcore_jni SHARED lb_jni.cpp)
target_include_directories(lbcore_jni PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lbcore_jni PRIVATE lb_core log)
