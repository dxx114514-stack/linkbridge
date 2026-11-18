package com.linkbridge.app

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.media.MediaScannerConnection
import android.os.Build
import android.os.Environment
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

/**
 * UI/服务层统一门面：所有 native 调用都切到 IO 线程；状态以 JSON 透传，
 * 由 Compose 侧按字段读取（避免在 Kotlin 侧复制一套核心数据模型）。
 */
object LinkBridgeEngine {
    @Volatile
    private var initialized = false
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    private val _running = MutableStateFlow(false)
    val running: StateFlow<Boolean> = _running

    private val _status = MutableStateFlow(JSONObject())
    val status: StateFlow<JSONObject> = _status

    private val _devices = MutableStateFlow<List<JSONObject>>(emptyList())
    val devices: StateFlow<List<JSONObject>> = _devices

    private val _conflicts = MutableStateFlow<List<JSONObject>>(emptyList())
    val conflicts: StateFlow<List<JSONObject>> = _conflicts

    private val _permissionCatalog = MutableStateFlow<List<JSONObject>>(emptyList())
    val permissionCatalog: StateFlow<List<JSONObject>> = _permissionCatalog

    private val _message = MutableStateFlow("")
    val message: StateFlow<String> = _message

    private val _confirmRequest = MutableStateFlow<JSONObject?>(null)
    val confirmRequest: StateFlow<JSONObject?> = _confirmRequest

    /** 实时屏幕流的最新一帧；为 null 表示未在查看。 */
    data class ScreenFrame(val bitmap: Bitmap, val seq: Long)

    private val _screenFrame = MutableStateFlow<ScreenFrame?>(null)
    val screenFrame: StateFlow<ScreenFrame?> = _screenFrame

    private val _events = MutableSharedFlow<JSONObject>(extraBufferCapacity = 128)
    val events: SharedFlow<JSONObject> = _events.asSharedFlow()

    /** Application / Activity / Service 都会调用，需可重入。 */
    private var appContext: Context? = null

    fun init(context: Context) {
        val ctx = context.applicationContext
        appContext = ctx
        if (initialized) return
        initialized = true
        NativeBridge.nativeSetPaths(ctx.filesDir.absolutePath, ctx.cacheDir.absolutePath)
        ClipboardBridge.init(ctx)
        NotifyBridge.init(ctx)
        scope.launch { refresh() }
    }

    suspend fun start(): String = withContext(Dispatchers.IO) {
        val name = Build.MODEL?.takeIf { it.isNotBlank() } ?: "android"
        val error = NativeBridge.nativeStart(name)
        _running.value = NativeBridge.nativeIsRunning()
        refresh()
        if (error.isBlank()) "" else error
    }

    suspend fun stop() = withContext(Dispatchers.IO) {
        NativeBridge.nativeStop()
        _running.value = false
        refresh()
    }

    suspend fun refresh() = withContext(Dispatchers.IO) {
        try {
            _status.value = JSONObject(NativeBridge.nativeStatusJson())
            _running.value = NativeBridge.nativeIsRunning()
        } catch (_: Exception) {
        }
        try {
            _devices.value = toList(NativeBridge.nativeDevicesJson())
        } catch (_: Exception) {
        }
        try {
            _conflicts.value = toList(NativeBridge.nativeConflictsJson())
        } catch (_: Exception) {
        }
        try {
            _permissionCatalog.value = toList(NativeBridge.nativePermissionCatalogJson())
        } catch (_: Exception) {
        }
    }

    suspend fun requestPairCode(ttlSec: Int = 300): JSONObject = withContext(Dispatchers.IO) {
        runCatching { JSONObject(NativeBridge.nativeRequestPairCode(ttlSec)) }
            .getOrDefault(JSONObject().put("error", "配对码生成失败"))
    }

    suspend fun pair(ip: String, code: String): String = withContext(Dispatchers.IO) {
        NativeBridge.nativePair(ip.trim(), code.trim()).also { refresh() }
    }

    suspend fun sendFile(peer: String, path: String, remoteName: String = ""): String =
        withContext(Dispatchers.IO) { NativeBridge.nativeSendFile(peer, path, remoteName) }

    /** 启动实时屏幕流（持续到 stopScreenStream 或超时）；返回空串表示已开始。 */
    suspend fun startScreenStream(peer: String, seconds: Int = 600): String =
        withContext(Dispatchers.IO) { NativeBridge.nativeStartScreenStream(peer, seconds) }

    fun stopScreenStream() {
        NativeBridge.nativeStopScreenStream()
        _screenFrame.value = null
    }

    /** 由 JNI 逐帧回调（后台线程）：转 Bitmap 后交给 UI。 */
    fun onScreenFrame(pixels: IntArray, width: Int, height: Int, seq: Long) {
        if (width <= 0 || height <= 0 || pixels.size < width * height) return
        val bitmap = try {
            Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888)
        } catch (_: Exception) {
            return
        }
        _screenFrame.value = ScreenFrame(bitmap, seq)
    }

    suspend fun syncNow(peer: String): String = withContext(Dispatchers.IO) {
        NativeBridge.nativeSyncNow(peer).also { refresh() }
    }

    suspend fun resolveConflict(path: String, action: String): String = withContext(Dispatchers.IO) {
        NativeBridge.nativeResolveConflict(path, action).also { refresh() }
    }

    suspend fun history(path: String): List<JSONObject> = withContext(Dispatchers.IO) {
        try {
            toList(NativeBridge.nativeHistoryJson(path))
        } catch (_: Exception) {
            emptyList()
        }
    }

    suspend fun restoreVersion(path: String, version: String): String = withContext(Dispatchers.IO) {
        NativeBridge.nativeRestoreVersion(path, version).also { refresh() }
    }

    suspend fun pushClipboard(text: String): String = withContext(Dispatchers.IO) {
        NativeBridge.nativePushClipboard(text)
    }

    /** 逐设备、逐权限位更新；即时生效并写入审计日志。 */
    suspend fun setPermissions(deviceId: String, mask: Long): String = withContext(Dispatchers.IO) {
        NativeBridge.nativeSetPermissions(deviceId, mask).also { refresh() }
    }

    /** 移除受信任设备（解除配对），立即生效。 */
    suspend fun removeDevice(deviceId: String): String = withContext(Dispatchers.IO) {
        NativeBridge.nativeRemoveDevice(deviceId).also { refresh() }
    }

    /** 运行时开关通知镜像（不写入配置文件）。 */
    fun setNotifyEnabled(value: Boolean) {
        NativeBridge.nativeSetNotifyEnabled(value)
        scope.launch { refresh() }
    }

    fun respondConfirm(id: Int, accept: Boolean) {
        NativeBridge.nativeRespondConfirm(id, accept)
        _confirmRequest.value = null
    }

    fun setAutoAccept(value: Boolean) {
        NativeBridge.nativeSetAutoAccept(value)
    }

    fun setMessage(text: String) {
        _message.value = text
    }

    /** 由 NativeBridge.onNativeEvent 调用（任意后台线程）。 */
    fun onNativeEvent(json: String) {
        val obj = try {
            JSONObject(json)
        } catch (_: Exception) {
            return
        }
        if (obj.optString("type") == "confirm_request") {
            _confirmRequest.value = obj
            return
        }
        _events.tryEmit(obj)
        when (obj.optString("type")) {
            "pairing_completed", "pairing_failed", "device_online", "device_offline",
            "sync_completed", "sync_failed", "clipboard_received", "clipboard_failed" ->
                scope.launch { refresh() }
            "screen_stream_ended" -> {
                _screenFrame.value = null
                val frames = obj.optInt("frames")
                val reason = obj.optString("reason")
                setMessage(
                    if (reason.isBlank()) "屏幕流已结束（$frames 帧）" else "屏幕流中断: $reason",
                )
            }
            "transfer_completed" -> {
                if (!obj.optBoolean("outgoing")) {
                    val path = obj.optString("path")
                    val name = obj.optString("file_name")
                    scope.launch {
                        val saved = saveIncomingFile(path, name)
                        if (saved != null) {
                            setMessage("已接收 $name，保存到 下载/LinkBridge")
                        } else {
                            val ctx = appContext
                            val legacyDenied = ctx != null &&
                                Build.VERSION.SDK_INT <= Build.VERSION_CODES.P &&
                                ContextCompat.checkSelfPermission(
                                    ctx,
                                    Manifest.permission.WRITE_EXTERNAL_STORAGE,
                                ) != PackageManager.PERMISSION_GRANTED
                            setMessage(
                                "已接收 $name（保存在应用私有目录 inbox" +
                                    if (legacyDenied) {
                                        "；请在系统设置授予存储权限以保存到「下载」）"
                                    } else {
                                        "）"
                                    },
                            )
                        }
                    }
                }
            }
        }
    }

    /** 把接收到的文件转存到系统「下载/LinkBridge」，成功返回展示用路径。 */
    private suspend fun saveIncomingFile(path: String, name: String): String? =
        withContext(Dispatchers.IO) {
            val ctx = appContext ?: return@withContext null
            val src = java.io.File(path)
            if (!src.exists() || name.isBlank()) return@withContext null
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                saveViaMediaStore(ctx, src, name)
            } else {
                saveToPublicDownloads(ctx, src, name)
            }
        }

    /** Android 10+：走分区存储的 Downloads 集合，无需存储权限。 */
    private fun saveViaMediaStore(ctx: Context, src: java.io.File, name: String): String? {
        val resolver = ctx.contentResolver
        val values = android.content.ContentValues().apply {
            put(android.provider.MediaStore.Downloads.DISPLAY_NAME, name)
            put(android.provider.MediaStore.Downloads.MIME_TYPE, "application/octet-stream")
            put(android.provider.MediaStore.Downloads.RELATIVE_PATH, "Download/LinkBridge")
            put(android.provider.MediaStore.Downloads.IS_PENDING, 1)
        }
        val uri = try {
            resolver.insert(android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
        } catch (_: Exception) {
            null
        } ?: return null
        return try {
            resolver.openOutputStream(uri)?.use { out ->
                src.inputStream().use { input -> input.copyTo(out) }
            } ?: return null
            values.clear()
            values.put(android.provider.MediaStore.Downloads.IS_PENDING, 0)
            resolver.update(uri, values, null, null)
            src.delete()
            "Download/LinkBridge/$name"
        } catch (_: Exception) {
            resolver.delete(uri, null, null)
            null
        }
    }

    /** Android 9 及以下：直接写公共下载目录，需要 WRITE_EXTERNAL_STORAGE。 */
    private fun saveToPublicDownloads(ctx: Context, src: java.io.File, name: String): String? {
        if (ContextCompat.checkSelfPermission(ctx, Manifest.permission.WRITE_EXTERNAL_STORAGE) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            return null
        }
        val dir = java.io.File(
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
            "LinkBridge",
        )
        if (!dir.exists() && !dir.mkdirs()) return null
        val target = uniqueTarget(dir, name)
        return try {
            src.inputStream().use { input -> target.outputStream().use { input.copyTo(it) } }
            src.delete()
            MediaScannerConnection.scanFile(ctx, arrayOf(target.absolutePath), null, null)
            "Download/LinkBridge/${target.name}"
        } catch (_: Exception) {
            target.delete()
            null
        }
    }

    private fun uniqueTarget(dir: java.io.File, name: String): java.io.File {
        var target = java.io.File(dir, name)
        if (!target.exists()) return target
        val dot = name.lastIndexOf('.')
        val base = if (dot > 0) name.substring(0, dot) else name
        val ext = if (dot > 0) name.substring(dot) else ""
        var index = 1
        while (target.exists()) {
            target = java.io.File(dir, "$base($index)$ext")
            index++
        }
        return target
    }

    private fun toList(raw: String): List<JSONObject> {
        val arr = JSONArray(raw)
        val out = ArrayList<JSONObject>(arr.length())
        for (i in 0 until arr.length()) arr.optJSONObject(i)?.let { out.add(it) }
        return out
    }
}
