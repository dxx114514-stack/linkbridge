package com.linkbridge.app

/**
 * JNI 桥：与 liblbcore_jni.so 中的 C++ 实现一一对应。
 * 所有 native 方法在 C++ 侧都是静态方法（JNI 的 jclass 形式）。
 */
object NativeBridge {
    init {
        System.loadLibrary("lbcore_jni")
    }

    @JvmStatic external fun nativeSetPaths(filesDir: String, cacheDir: String)
    @JvmStatic external fun nativeStart(deviceName: String): String
    @JvmStatic external fun nativeStop()
    @JvmStatic external fun nativeIsRunning(): Boolean

    @JvmStatic external fun nativeIdentityJson(): String
    @JvmStatic external fun nativeDevicesJson(): String
    @JvmStatic external fun nativeStatusJson(): String

    @JvmStatic external fun nativeRequestPairCode(ttlSec: Int): String
    @JvmStatic external fun nativePair(ip: String, code: String): String

    @JvmStatic external fun nativeSendFile(peer: String, path: String, remoteName: String): String
    @JvmStatic external fun nativeSyncNow(peer: String): String
    @JvmStatic external fun nativeConflictsJson(): String
    @JvmStatic external fun nativeResolveConflict(path: String, action: String): String
    @JvmStatic external fun nativeHistoryJson(path: String): String
    @JvmStatic external fun nativeRestoreVersion(path: String, version: String): String

    @JvmStatic external fun nativePushClipboard(text: String): String

    @JvmStatic external fun nativeRespondConfirm(id: Int, accept: Boolean)
    @JvmStatic external fun nativeSetAutoAccept(value: Boolean)

    /** C++ 事件回调入口（JSON 文本）。 */
    @JvmStatic
    fun onNativeEvent(json: String) {
        LinkBridgeEngine.onNativeEvent(json)
    }
}
