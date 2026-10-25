package com.linkbridge.app

import android.accessibilityservice.AccessibilityService
import android.view.accessibility.AccessibilityEvent

/**
 * 无障碍服务：只为远程控制提供输入注入能力（手势与系统按键），
 * 不读取窗口内容、不监听文本；启用入口在应用「屏幕控制」卡片中。
 */
class LinkBridgeAccessibilityService : AccessibilityService() {
    override fun onServiceConnected() {
        super.onServiceConnected()
        InputBridge.attach(this)
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {
        // 输入注入不需要事件流，保持空实现以降低开销。
    }

    override fun onInterrupt() {
        InputBridge.detach(this)
    }

    override fun onDestroy() {
        InputBridge.detach(this)
        super.onDestroy()
    }
}
