package com.linkbridge.app

import android.content.Context

/**
 * 「总是允许」策略：按功能分别持久化，开启后该功能不再弹确认框，直接允许。
 *
 * 只影响「是否询问用户」，不改变设备权限位，也不能替代系统授权（投屏、通知使用权等）。
 * 原生层的开关状态保存在进程内存中，因此服务重启或新进程启动时需要调用 [applyAll] 回填。
 */
object AutoAllowPrefs {
    private const val NAME = "auto_allow"

    const val PAIRING = "pairing"
    const val CLIPBOARD = "clipboard"
    const val SCREEN = "screen"
    const val NOTIFY = "notify_action"
    const val TRANSFER = "transfer"

    val ALL = listOf(PAIRING, CLIPBOARD, SCREEN, NOTIFY, TRANSFER)

    private fun prefs(ctx: Context) = ctx.getSharedPreferences(NAME, Context.MODE_PRIVATE)

    fun isEnabled(ctx: Context, kind: String): Boolean = prefs(ctx).getBoolean(kind, false)

    fun setEnabled(ctx: Context, kind: String, value: Boolean) {
        prefs(ctx).edit().putBoolean(kind, value).apply()
        NativeBridge.nativeSetAutoAllow(kind, value)
    }

    /** 把已保存的策略同步给原生层（服务重启、新进程启动、应用回到前台时调用）。 */
    fun applyAll(ctx: Context) {
        ALL.forEach { NativeBridge.nativeSetAutoAllow(it, isEnabled(ctx, it)) }
    }

    fun title(kind: String): String = when (kind) {
        PAIRING -> "自动接受配对请求"
        CLIPBOARD -> "自动接受剪贴板"
        SCREEN -> "自动接受屏幕查看"
        NOTIFY -> "自动执行通知动作"
        TRANSFER -> "自动接收文件"
        else -> kind
    }

    fun description(kind: String): String = when (kind) {
        PAIRING -> "对端发起配对时不再弹窗，直接完成配对（仍校验配对码，但不再人工核对指纹）"
        CLIPBOARD -> "收到对端剪贴板时直接写入本机剪贴板，不再逐个确认"
        SCREEN -> "对端请求查看本机屏幕时直接同意（仍需系统投屏授权，且需开启共享）"
        NOTIFY -> "点击镜像通知时直接执行动作，不再确认（可能触发自动点击）"
        TRANSFER -> "对端发来文件时直接接收，不再逐个确认"
        else -> ""
    }
}
