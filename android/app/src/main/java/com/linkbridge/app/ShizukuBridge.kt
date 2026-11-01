package com.linkbridge.app

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.util.Log

/**
 * Shizuku 适配层：全程反射调用 rikka.shizuku.Shizuku，未集成该依赖时不会崩溃，
 * 而是返回明确的降级状态，由界面提示并回退到无障碍注入。
 *
 * 状态机：UNSUPPORTED（未集成 API）→ NOT_INSTALLED（未安装 Shizuku）
 *        → NOT_RUNNING（服务未启动）→ PERMISSION_REQUIRED（未授权）
 *        → READY（可执行系统级命令）。
 */
object ShizukuBridge {
    private const val TAG = "LBShizuku"
    private const val SHIZUKU_PACKAGE = "moe.shizuku.privileged.api"
    private const val PERMISSION_GRANTED = 0

    enum class State {
        UNSUPPORTED,
        NOT_INSTALLED,
        NOT_RUNNING,
        PERMISSION_REQUIRED,
        READY,
    }

    private fun shizukuClass(): Class<*>? = try {
        Class.forName("rikka.shizuku.Shizuku")
    } catch (_: Throwable) {
        null
    }

    fun isInstalled(context: Context): Boolean = try {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.packageManager.getPackageInfo(
                SHIZUKU_PACKAGE,
                PackageManager.PackageInfoFlags.of(0),
            )
        } else {
            @Suppress("DEPRECATION")
            context.packageManager.getPackageInfo(SHIZUKU_PACKAGE, 0)
        }
        true
    } catch (_: Exception) {
        false
    }

    fun state(context: Context): State {
        val cls = shizukuClass() ?: return State.UNSUPPORTED
        return try {
            val pinged = cls.getMethod("pingBinder").invoke(null) as? Boolean ?: false
            if (!pinged) return State.NOT_RUNNING
            val permission = (cls.getMethod("checkSelfPermission").invoke(null) as? Int) ?: -1
            if (permission == PERMISSION_GRANTED) State.READY else State.PERMISSION_REQUIRED
        } catch (e: Throwable) {
            Log.w(TAG, "Shizuku 状态检测异常", e)
            State.NOT_RUNNING
        }
    }

    fun statusText(context: Context): String = when (state(context)) {
        State.READY -> "Shizuku 已就绪：系统级动作可直接执行"
        State.PERMISSION_REQUIRED -> "Shizuku 已运行但未授权：请在 Shizuku 中允许连桥"
        State.NOT_RUNNING -> "Shizuku 未运行：请启动 Shizuku 服务后重试"
        State.NOT_INSTALLED -> "未安装 Shizuku：可安装后获得更稳定的系统级动作"
        State.UNSUPPORTED -> "当前安装包未集成 Shizuku API：将自动使用无障碍注入降级"
    }

    /** 引导：优先打开 Shizuku 应用，其次打开其官网/应用商店。 */
    fun openShizuku(context: Context) {
        if (isInstalled(context)) {
            val launch = context.packageManager.getLaunchIntentForPackage(SHIZUKU_PACKAGE)
            if (launch != null) {
                launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                runCatching { context.startActivity(launch) }
                return
            }
        }
        val market = Intent(
            Intent.ACTION_VIEW,
            Uri.parse("market://details?id=$SHIZUKU_PACKAGE"),
        ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (runCatching { context.startActivity(market) }.isFailure) {
            val web = Intent(
                Intent.ACTION_VIEW,
                Uri.parse("https://shizuku.rikka.app/"),
            ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            runCatching { context.startActivity(web) }
        }
    }

    /**
     * 申请授权：仅在集成 Shizuku API 时可用；未集成时返回 false，
     * 由界面提示用户到 Shizuku 应用内手动授权。
     */
    fun requestPermission(): Boolean {
        val cls = shizukuClass() ?: return false
        return try {
            val pinged = cls.getMethod("pingBinder").invoke(null) as? Boolean ?: false
            if (!pinged) return false
            cls.getMethod("requestPermission", Int::class.javaPrimitiveType)
                .invoke(null, 0)
            true
        } catch (e: Throwable) {
            Log.w(TAG, "Shizuku 授权请求失败", e)
            false
        }
    }

    /** 通过 Shizuku 执行 `input tap x y`；不可用时返回 false，由调用方降级。 */
    fun tap(x: Int, y: Int): Boolean {
        if (x < 0 || y < 0) return false
        val process = newProcess(arrayOf("input", "tap", x.toString(), y.toString())) ?: return false
        return try {
            val finished = process.waitFor(3, java.util.concurrent.TimeUnit.SECONDS)
            finished && process.exitValue() == 0
        } catch (e: Throwable) {
            Log.w(TAG, "Shizuku 点击执行失败", e)
            false
        } finally {
            runCatching { process.destroy() }
        }
    }

    private fun newProcess(cmd: Array<String>): Process? {
        val cls = shizukuClass() ?: return null
        return try {
            val pinged = cls.getMethod("pingBinder").invoke(null) as? Boolean ?: false
            if (!pinged) return null
            val permission = (cls.getMethod("checkSelfPermission").invoke(null) as? Int) ?: -1
            if (permission != PERMISSION_GRANTED) return null
            cls.getMethod(
                "newProcess",
                Array<String>::class.java,
                Array<String>::class.java,
                String::class.java,
            ).invoke(null, cmd, null, null) as? Process
        } catch (e: Throwable) {
            Log.w(TAG, "Shizuku newProcess 不可用", e)
            null
        }
    }
}
