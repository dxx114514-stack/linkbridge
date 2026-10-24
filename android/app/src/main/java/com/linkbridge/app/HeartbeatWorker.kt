package com.linkbridge.app

import android.content.Context
import androidx.work.Worker
import androidx.work.WorkerParameters

/**
 * WorkManager 周期巡检：进程被杀后尝试恢复前台服务，并刷新一次状态。
 * 后台启动前台服务在 Android 12+ 可能被系统拒绝，此时静默失败，等待用户下次打开应用。
 */
class HeartbeatWorker(context: Context, params: WorkerParameters) : Worker(context, params) {
    override fun doWork(): Result {
        return try {
            if (!NativeBridge.nativeIsRunning()) {
                LinkBridgeService.start(applicationContext)
            }
            Result.success()
        } catch (_: Exception) {
            Result.retry()
        }
    }
}
