package com.linkbridge.app

import android.content.ClipboardManager
import android.content.Context
import android.os.Handler
import android.os.Looper
import java.security.MessageDigest

/**
 * 剪贴板自动获取与推送：用户不再需要手动输入或粘贴文本。
 * - 自动模式：应用回到前台时读取当前剪贴板，内容变化才推送（对端按哈希幂等，不会重复弹确认）；
 * - 立即同步：悬浮球双击 / 界面按钮触发，忽略去重直接推送一次。
 * Android 10+ 限制后台读取剪贴板，读取失败时给出明确提示而不是静默跳过。
 */
object ClipboardAutoSync {
    private const val PREFS = "clipboard_auto"
    private const val KEY_AUTO = "auto_enabled"
    private const val KEY_LAST_DIGEST = "last_digest"

    @Volatile
    private var busy = false

    private fun prefs(context: Context) =
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun isAutoEnabled(context: Context): Boolean = prefs(context).getBoolean(KEY_AUTO, true)

    fun setAutoEnabled(context: Context, value: Boolean) {
        prefs(context).edit().putBoolean(KEY_AUTO, value).apply()
    }

    /** 读取当前剪贴板文本；为空或不可读（后台限制）时返回 null。 */
    fun readClipboard(context: Context): String? = try {
        val cb = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        if (!cb.hasPrimaryClip()) {
            null
        } else {
            val clip = cb.primaryClip
            if (clip == null || clip.itemCount == 0) null
            else clip.getItemAt(0).coerceToText(context)?.toString()?.takeIf { it.isNotBlank() }
        }
    } catch (_: Exception) {
        null
    }

    /** 供界面展示的剪贴板预览（最多 60 字）。 */
    fun preview(context: Context): String? = readClipboard(context)?.let {
        if (it.length > 60) it.take(60) + "…" else it
    }

    /** 记录来自对端并已写入本机剪贴板的内容，避免自动模式把它再推回对端。 */
    fun recordRemote(context: Context, text: String) {
        prefs(context).edit().putString(KEY_LAST_DIGEST, digest(text)).apply()
    }

    /**
     * 后台读取并推送剪贴板；[force] 为 false 时相同内容不重复推送。
     * 结果通过 [onResult] 回主线程（可为空）。
     */
    fun sync(context: Context, force: Boolean, onResult: ((String) -> Unit)? = null) {
        val ctx = context.applicationContext
        synchronized(this) {
            if (busy) {
                onResult?.invoke("正在同步中，请稍候")
                return
            }
            busy = true
        }
        Thread {
            val result = try {
                val text = readClipboard(ctx)
                when {
                    text == null -> "剪贴板为空或不可读（请保持连桥在前台后重试）"
                    !force && digest(text) == prefs(ctx).getString(KEY_LAST_DIGEST, "") -> "剪贴板未变化"
                    else -> {
                        val error = NativeBridge.nativePushClipboard(text)
                        if (error.isBlank()) {
                            prefs(ctx).edit().putString(KEY_LAST_DIGEST, digest(text)).apply()
                            "已同步剪贴板（${text.length} 字）"
                        } else {
                            "同步失败：$error"
                        }
                    }
                }
            } catch (e: Exception) {
                "同步失败：${e.message ?: e.javaClass.simpleName}"
            } finally {
                synchronized(this) { busy = false }
            }
            Handler(Looper.getMainLooper()).post { onResult?.invoke(result) }
        }.start()
    }

    private fun digest(text: String): String =
        MessageDigest.getInstance("SHA-256").digest(text.toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it) }
}
