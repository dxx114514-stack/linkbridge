package com.linkbridge.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import java.util.concurrent.TimeUnit

/**
 * 数据同步前台服务：持有 Node 的监听线程（配对/心跳/传输/剪贴板/屏幕/同步）。
 * 退出后台后仍可持续工作；WorkManager 周期巡检兜底。
 */
class LinkBridgeService : Service() {
    companion object {
        private const val CHANNEL_ID = "linkbridge_sync"
        private const val NOTIFICATION_ID = 4100
        private const val WORK_NAME = "linkbridge-heartbeat"

        fun start(context: Context) {
            val intent = Intent(context, LinkBridgeService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, LinkBridgeService::class.java))
        }

        fun scheduleHeartbeat(context: Context) {
            val request = PeriodicWorkRequestBuilder<HeartbeatWorker>(15, TimeUnit.MINUTES)
                .setInitialDelay(10, TimeUnit.MINUTES)
                .build()
            WorkManager.getInstance(context).enqueueUniquePeriodicWork(
                WORK_NAME,
                ExistingPeriodicWorkPolicy.KEEP,
                request,
            )
        }
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        LinkBridgeEngine.init(applicationContext)
        ensureChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForegroundCompat()
        scope.launch {
            val error = LinkBridgeEngine.start()
            val text = if (error.isBlank()) "服务运行中" else "启动失败：$error"
            LinkBridgeEngine.setMessage(text)
            updateNotification(text)
        }
        Companion.scheduleHeartbeat(applicationContext)
        return START_STICKY
    }

    override fun onDestroy() {
        runBlocking { LinkBridgeEngine.stop() }
        scope.cancel()
        super.onDestroy()
    }

    private fun startForegroundCompat() {
        val notification = buildNotification("服务启动中…")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun updateNotification(text: String) {
        val manager = getSystemService(NotificationManager::class.java)
        manager.notify(NOTIFICATION_ID, buildNotification(text))
    }

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val manager = getSystemService(NotificationManager::class.java)
        if (manager.getNotificationChannel(CHANNEL_ID) == null) {
            manager.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, getString(R.string.channel_sync), NotificationManager.IMPORTANCE_LOW),
            )
        }
    }

    private fun buildNotification(text: String): Notification {
        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_ID)
        } else {
            Notification.Builder(this)
        }
        return builder
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_download_done)
            .setOngoing(true)
            .build()
    }
}
