package com.linkbridge.app

import android.content.Context
import android.content.SharedPreferences

/**
 * 悬浮球参数持久化（同一进程内 UI 与服务共享）。
 * 值全部采用简单类型，改动后通过 [save] 触发服务刷新。
 */
object FloatingBallPrefs {
    private const val FILE = "floating_ball"
    private const val KEY_ENABLED = "enabled"
    private const val KEY_X = "x"
    private const val KEY_Y = "y"
    private const val KEY_SIZE = "size_dp"
    private const val KEY_ALPHA = "alpha"
    private const val KEY_COLOR = "color"
    private const val KEY_CLICK = "click_action"
    private const val KEY_LONG = "long_action"
    private const val KEY_DOUBLE = "double_action"
    private const val KEY_VISIBILITY = "visibility"
    private const val KEY_HIDDEN_APPS = "hidden_apps"
    private const val KEY_SCHEME = "scheme"
    // 2：单击默认改为功能轮盘，双击默认改为快捷剪贴板同步（旧配置在下次加载时迁移）
    private const val SCHEME = 2

    const val CLICK_WHEEL = "wheel"
    const val CLICK_OPEN_APP = "open_app"
    const val CLICK_TOGGLE_SERVICE = "toggle_service"
    const val CLICK_NONE = "none"

    const val DOUBLE_QUICK_CLIP = "quick_clip"
    const val DOUBLE_NONE = "none"

    const val LONG_OPEN_SETTINGS = "settings"
    const val LONG_TOGGLE_SERVICE = "toggle_service"
    const val LONG_HIDE = "hide"

    const val VISIBILITY_ALWAYS = "always"
    const val VISIBILITY_RUNNING = "running"

    data class Settings(
        val enabled: Boolean = false,
        val x: Int = -1,
        val y: Int = -1,
        val sizeDp: Int = 56,
        val alpha: Float = 0.85f,
        val color: Int = 0xFF3F51B5.toInt(),
        val clickAction: String = CLICK_WHEEL,
        val doubleClickAction: String = DOUBLE_QUICK_CLIP,
        val longPressAction: String = LONG_TOGGLE_SERVICE,
        val visibility: String = VISIBILITY_ALWAYS,
        val hiddenApps: Set<String> = emptySet(),
    )

    private fun prefs(context: Context): SharedPreferences =
        context.applicationContext.getSharedPreferences(FILE, Context.MODE_PRIVATE)

    fun load(context: Context): Settings {
        val p = prefs(context)
        val migrated = p.getInt(KEY_SCHEME, 1) < SCHEME
        return Settings(
            enabled = p.getBoolean(KEY_ENABLED, false),
            x = p.getInt(KEY_X, -1),
            y = p.getInt(KEY_Y, -1),
            sizeDp = p.getInt(KEY_SIZE, 56).coerceIn(40, 88),
            alpha = p.getFloat(KEY_ALPHA, 0.85f).coerceIn(0.2f, 1.0f),
            color = p.getInt(KEY_COLOR, 0xFF3F51B5.toInt()),
            clickAction = if (migrated) CLICK_WHEEL
            else p.getString(KEY_CLICK, CLICK_WHEEL) ?: CLICK_WHEEL,
            doubleClickAction = if (migrated) DOUBLE_QUICK_CLIP
            else p.getString(KEY_DOUBLE, DOUBLE_QUICK_CLIP) ?: DOUBLE_QUICK_CLIP,
            longPressAction = p.getString(KEY_LONG, LONG_TOGGLE_SERVICE) ?: LONG_TOGGLE_SERVICE,
            visibility = p.getString(KEY_VISIBILITY, VISIBILITY_ALWAYS) ?: VISIBILITY_ALWAYS,
            hiddenApps = p.getStringSet(KEY_HIDDEN_APPS, emptySet())?.toSet() ?: emptySet(),
        )
    }

    fun save(context: Context, settings: Settings) {
        prefs(context).edit()
            .putBoolean(KEY_ENABLED, settings.enabled)
            .putInt(KEY_X, settings.x)
            .putInt(KEY_Y, settings.y)
            .putInt(KEY_SIZE, settings.sizeDp.coerceIn(40, 88))
            .putFloat(KEY_ALPHA, settings.alpha.coerceIn(0.2f, 1.0f))
            .putInt(KEY_COLOR, settings.color)
            .putString(KEY_CLICK, settings.clickAction)
            .putString(KEY_DOUBLE, settings.doubleClickAction)
            .putString(KEY_LONG, settings.longPressAction)
            .putString(KEY_VISIBILITY, settings.visibility)
            .putStringSet(KEY_HIDDEN_APPS, settings.hiddenApps)
            .putInt(KEY_SCHEME, SCHEME)
            .apply()
    }

    fun update(context: Context, block: (Settings) -> Settings): Settings {
        val next = block(load(context))
        save(context, next)
        return next
    }
}
