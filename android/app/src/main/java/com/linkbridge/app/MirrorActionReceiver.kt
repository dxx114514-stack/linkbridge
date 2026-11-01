package com.linkbridge.app

import android.app.NotificationManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.widget.Toast

/**
 * 镜像通知点击入口：把点击回传给来源设备（B），由 B 执行预设动作。
 * 网络请求可能阻塞，因此使用 goAsync + 后台线程，避免拖住主线程。
 */
class MirrorActionReceiver : BroadcastReceiver() {
    companion object {
        const val EXTRA_SOURCE = "source"
        const val EXTRA_NOTIFICATION_ID = "notification_id"
        const val EXTRA_APP = "app"
        const val EXTRA_APP_NAME = "app_name"
        const val EXTRA_TITLE = "title"
        const val EXTRA_NOTIFY_ID = "notify_id"
    }

    override fun onReceive(context: Context, intent: Intent) {
        val pending = goAsync()
        val source = intent.getStringExtra(EXTRA_SOURCE).orEmpty()
        val notificationId = intent.getStringExtra(EXTRA_NOTIFICATION_ID).orEmpty()
        val app = intent.getStringExtra(EXTRA_APP).orEmpty()
        val appName = intent.getStringExtra(EXTRA_APP_NAME).orEmpty()
        val title = intent.getStringExtra(EXTRA_TITLE).orEmpty()
        val notifyId = intent.getIntExtra(EXTRA_NOTIFY_ID, 0)
        if (notifyId != 0) {
            runCatching {
                val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
                manager.cancel(notifyId)
            }
        }
        if (source.isBlank() || notificationId.isBlank()) {
            pending.finish()
            return
        }
        Thread {
            val error = try {
                NativeBridge.nativeNotifyAction(source, notificationId, app, appName, title)
            } catch (e: Exception) {
                "调用失败: ${e.message ?: e.javaClass.simpleName}"
            }
            if (error.isNotBlank()) {
                Handler(Looper.getMainLooper()).post {
                    Toast.makeText(context, "来源设备未执行动作：$error", Toast.LENGTH_LONG).show()
                }
            }
            pending.finish()
        }.start()
    }
}
