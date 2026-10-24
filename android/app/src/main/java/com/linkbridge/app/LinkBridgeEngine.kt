package com.linkbridge.app

import android.content.Context
import android.os.Build
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

    private val _message = MutableStateFlow("")
    val message: StateFlow<String> = _message

    private val _confirmRequest = MutableStateFlow<JSONObject?>(null)
    val confirmRequest: StateFlow<JSONObject?> = _confirmRequest

    private val _events = MutableSharedFlow<JSONObject>(extraBufferCapacity = 128)
    val events: SharedFlow<JSONObject> = _events.asSharedFlow()

    /** Application / Activity / Service 都会调用，需可重入。 */
    fun init(context: Context) {
        val ctx = context.applicationContext
        if (initialized) return
        initialized = true
        NativeBridge.nativeSetPaths(ctx.filesDir.absolutePath, ctx.cacheDir.absolutePath)
        ClipboardBridge.init(ctx)
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
        }
    }

    private fun toList(raw: String): List<JSONObject> {
        val arr = JSONArray(raw)
        val out = ArrayList<JSONObject>(arr.length())
        for (i in 0 until arr.length()) arr.optJSONObject(i)?.let { out.add(it) }
        return out
    }
}
