package com.linkbridge.app

import android.app.Activity
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.view.Gravity
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast

/**
 * 悬浮球双击触发的快捷窗口（透明主题，用完即退）：
 * 前台获得剪贴板读取权限 → 自动获取剪贴板 → 推送到已授权设备 → 提示后退出。
 * Android 10+ 后台无法读取剪贴板，所以必须短暂进入前台再读。
 */
class QuickClipActivity : Activity() {
    private var started = false
    private var label: TextView? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        LinkBridgeEngine.init(applicationContext)
        val text = TextView(this).apply {
            this.text = "正在同步剪贴板…"
            setTextColor(Color.WHITE)
            textSize = 16f
            gravity = Gravity.CENTER
            setPadding(dp(24), dp(14), dp(24), dp(14))
            background = GradientDrawable().apply {
                cornerRadius = dp(22).toFloat()
                setColor(0xE63F51B5.toInt())
            }
        }
        label = text
        setContentView(
            FrameLayout(this).apply {
                addView(
                    text,
                    FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        Gravity.CENTER,
                    ),
                )
            },
        )
    }

    override fun onResume() {
        super.onResume()
        if (started) return
        started = true
        if (!LinkBridgeEngine.running.value) {
            LinkBridgeService.start(this)
            Toast.makeText(this, "同步服务未启动，已为你启动，请稍后再双击悬浮球", Toast.LENGTH_SHORT).show()
            finish()
            return
        }
        ClipboardAutoSync.sync(this, force = true) { message ->
            label?.text = message
            Toast.makeText(this, message, Toast.LENGTH_SHORT).show()
            finish()
        }
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()
}
