package com.linkbridge.app

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import java.util.concurrent.atomic.AtomicLong

/**
 * 剪贴板桥：只支持文本（Android 端首版）。C++ 侧轮询 sequence() 判断变化。
 * 注意：Android 10+ 限制后台读取剪贴板，需应用处于前台或前台服务中。
 */
object ClipboardBridge {
    private var appContext: Context? = null
    private val sequence = AtomicLong(0)
    private var listener: ClipboardManager.OnPrimaryClipChangedListener? = null

    @JvmStatic
    fun init(context: Context) {
        val ctx = context.applicationContext
        appContext = ctx
        if (listener == null) {
            val cb = clipboard(ctx)
            val l = ClipboardManager.OnPrimaryClipChangedListener { sequence.incrementAndGet() }
            cb.addPrimaryClipChangedListener(l)
            listener = l
        }
    }

    private fun clipboard(context: Context): ClipboardManager =
        context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager

    @JvmStatic
    fun sequence(): Long = sequence.get()

    @JvmStatic
    fun readText(): ByteArray? {
        val ctx = appContext ?: return null
        return try {
            val cb = clipboard(ctx)
            if (!cb.hasPrimaryClip()) return null
            val clip = cb.primaryClip ?: return null
            if (clip.itemCount == 0) return null
            val text = clip.getItemAt(0).coerceToText(ctx)?.toString() ?: return null
            text.toByteArray(Charsets.UTF_8)
        } catch (e: Exception) {
            null
        }
    }

    @JvmStatic
    fun writeText(data: ByteArray): Boolean {
        val ctx = appContext ?: return false
        return try {
            val text = String(data, Charsets.UTF_8)
            clipboard(ctx).setPrimaryClip(ClipData.newPlainText("LinkBridge", text))
            sequence.incrementAndGet()
            true
        } catch (e: Exception) {
            false
        }
    }
}
