package com.linkbridge.app

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.AccessibilityServiceInfo
import android.accessibilityservice.GestureDescription
import android.content.Context
import android.content.Intent
import android.graphics.Path
import android.provider.Settings
import android.view.accessibility.AccessibilityManager

/**
 * 输入注入桥：由 LinkBridgeAccessibilityService 在 onServiceConnected 时挂载自身。
 * 核心层经 JNI 调用 [inject]，坐标使用被控端画面像素。
 */
object InputBridge {
    private const val TYPE_TAP = 1
    private const val TYPE_SWIPE = 2
    private const val TYPE_KEY = 3

    @Volatile
    private var service: AccessibilityService? = null

    fun attach(s: AccessibilityService) {
        service = s
    }

    fun detach(s: AccessibilityService) {
        if (service === s) service = null
    }

    /** 无障碍服务是否已连接（已启用且在运行）。 */
    @JvmStatic
    fun isReady(): Boolean = service != null

    /** 当前应用的无障碍服务是否在系统设置中启用（用于界面状态展示）。 */
    fun isEnabled(context: Context): Boolean {
        val manager = context.getSystemService(Context.ACCESSIBILITY_SERVICE) as? AccessibilityManager
            ?: return false
        val services = manager.getEnabledAccessibilityServiceList(
            AccessibilityServiceInfo.FEEDBACK_ALL_MASK,
        )
        val self = LinkBridgeAccessibilityService::class.java.name
        return services.any {
            it.resolveInfo?.serviceInfo?.let { info ->
                info.packageName == context.packageName && info.name == self
            } ?: false
        }
    }

    /** 跳转系统无障碍设置页，由用户在「已下载的应用」中启用连桥。 */
    fun openSettings(context: Context) {
        val intent = Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        runCatching { context.startActivity(intent) }
    }

    /**
     * 注入一次输入事件（type: 1=点击 2=滑动 3=系统按键）。
     * 返回 false 表示服务未连接或系统拒绝本次注入。
     */
    @JvmStatic
    fun inject(type: Int, x1: Int, y1: Int, x2: Int, y2: Int, extra: Int): Boolean {
        val svc = service ?: return false
        return when (type) {
            // 点击用 50ms 短手势：1ms 在部分 ROM 上可能不被识别为点击。
            TYPE_TAP -> gesture(svc, x1, y1, x1, y1, 50)
            TYPE_SWIPE -> gesture(svc, x1, y1, x2, y2, if (extra > 0) extra else 200)
            TYPE_KEY -> when (extra) {
                1 -> svc.performGlobalAction(AccessibilityService.GLOBAL_ACTION_BACK)
                2 -> svc.performGlobalAction(AccessibilityService.GLOBAL_ACTION_HOME)
                3 -> svc.performGlobalAction(AccessibilityService.GLOBAL_ACTION_RECENTS)
                else -> false
            }
            else -> false
        }
    }

    private fun gesture(
        svc: AccessibilityService,
        x1: Int,
        y1: Int,
        x2: Int,
        y2: Int,
        durationMs: Int,
    ): Boolean {
        val path = Path().apply {
            moveTo(x1.toFloat(), y1.toFloat())
            if (x1 != x2 || y1 != y2) lineTo(x2.toFloat(), y2.toFloat())
        }
        val stroke = GestureDescription.StrokeDescription(path, 0, durationMs.toLong())
        val gesture = GestureDescription.Builder().addStroke(stroke).build()
        return svc.dispatchGesture(gesture, null, null)
    }
}
