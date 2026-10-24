package com.linkbridge.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.IBinder

/**
 * 屏幕共享前台服务：MediaProjection 必须在 startForeground 之后获取，
 * 并把授权结果交给 ScreenCaptureBridge 建立 VirtualDisplay。
 */
class ScreenShareService : Service() {
    companion object {
        private const val CHANNEL_ID = "linkbridge_screen"
        private const val NOTIFICATION_ID = 4101
        private const val EXTRA_RESULT_CODE = "result_code"
        private const val EXTRA_RESULT_DATA = "result_data"
        private const val ACTION_STOP = "com.linkbridge.app.action.STOP_SCREEN"

        fun start(context: Context, resultCode: Int, data: Intent) {
            val intent = Intent(context, ScreenShareService::class.java).apply {
                putExtra(EXTRA_RESULT_CODE, resultCode)
                putExtra(EXTRA_RESULT_DATA, data)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, ScreenShareService::class.java))
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        ensureChannel()
        val notification = buildNotification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }

        val resultCode = intent?.getIntExtra(EXTRA_RESULT_CODE, 0) ?: 0
        val data = intent?.let { readParcelable(it) }
        if (resultCode == 0 || data == null) {
            stopSelf()
            return START_NOT_STICKY
        }

        val manager = getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        val projection = try {
            manager.getMediaProjection(resultCode, data)
        } catch (_: Exception) {
            null
        }
        if (projection == null) {
            stopSelf()
            return START_NOT_STICKY
        }
        val metrics = resources.displayMetrics
        ScreenCaptureBridge.attach(projection, metrics.widthPixels, metrics.heightPixels, metrics.densityDpi)
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        ScreenCaptureBridge.release()
        super.onDestroy()
    }

    @Suppress("DEPRECATION")
    private fun readParcelable(intent: Intent): Intent? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(EXTRA_RESULT_DATA, Intent::class.java)
        } else {
            intent.getParcelableExtra(EXTRA_RESULT_DATA)
        }

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val manager = getSystemService(NotificationManager::class.java)
        if (manager.getNotificationChannel(CHANNEL_ID) == null) {
            manager.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, getString(R.string.channel_screen), NotificationManager.IMPORTANCE_LOW),
            )
        }
    }

    private fun buildNotification(): Notification {
        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_ID)
        } else {
            Notification.Builder(this)
        }
        return builder
            .setContentTitle(getString(R.string.app_name))
            .setContentText("屏幕共享进行中（只读）")
            .setSmallIcon(android.R.drawable.ic_menu_view)
            .setOngoing(true)
            .build()
    }
}
