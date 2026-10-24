#pragma once
// Android JNI 公共设施：JVM 缓存、类引用缓存、线程附着与 Java/Kotlin 数据转换。
// 仅 Android 构建使用（核心库与 JNI 共享库共用）。
#include <jni.h>

#include <string>

#include "core/common/types.h"

namespace lb::android {

// JNI_OnLoad 调用；只保存 JavaVM
bool init(JavaVM* vm);

// 当前线程的 JNIEnv（必要时 AttachCurrentThread）。与 release 成对使用。
JNIEnv* env();
void release(JNIEnv* e);

// 在 Java 线程上调用，预先解析 app 侧桥接类，避免后台线程 FindClass 失败
bool ensure_classes(JNIEnv* e);

// 取已缓存的类引用（ensure_classes 之后可用）；未缓存时返回 nullptr
jclass class_ref(const char* name);

std::string to_string(JNIEnv* e, jstring s);
jstring to_jstring(JNIEnv* e, const std::string& s);
jbyteArray to_jbytes(JNIEnv* e, const ByteBuf& data);
ByteBuf from_jbytes(JNIEnv* e, jbyteArray arr);

// 在 Java 侧抛出 IllegalStateException（JNI 方法返回前调用）
void throw_java(JNIEnv* e, const std::string& message);

// 清空类引用缓存（进程退出用）
void shutdown(JNIEnv* e);

} // namespace lb::android
