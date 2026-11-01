package com.linkbridge.app

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.provider.Settings
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.text.TextUtils
import android.util.Log

/**
 * 通知使用权监听：把本机通知（除本应用自身）转交 native 核心，
 * 由核心按设备权限同步到其他设备。忽略群组摘要、常驻通知和媒体通知，减少噪声。
 */
class LinkBridgeNotificationListener : NotificationListenerService() {
    companion object {
        private const val TAG = "LBNotifyListener"

        /** 最近通知的原始点击意图缓存（key=sbn.key），用于跨设备回唤。 */
        private val recentIntents = LinkedHashMap<String, PendingIntent>()
        private const val INTENT_CACHE_LIMIT = 32

        @Volatile
        var connected: Boolean = false
            private set

        /**
         * 在来源设备上唤起原始通知（调用通知自带的 contentIntent）。
         * 无缓存、意图失效或被系统拒绝时返回 false，由调用方降级为打开应用。
         */
        @JvmStatic
        fun fireOriginalIntent(id: String): Boolean {
            val pending = synchronized(recentIntents) { recentIntents[id] } ?: return false
            return try {
                pending.send()
                true
            } catch (e: Exception) {
                Log.w(TAG, "唤起原始通知失败: $id", e)
                false
            }
        }

        /** 通知使用权是否已授予（用于界面展示）。 */
        @JvmStatic
        fun isEnabled(context: Context): Boolean {
            if (connected) return true
            val flat = Settings.Secure.getString(
                context.contentResolver,
                "enabled_notification_listeners",
            ) ?: return false
            val component = "${context.packageName}/${LinkBridgeNotificationListener::class.java.name}"
            return flat.split(':').any { it.equals(component, ignoreCase = true) }
        }

        fun openSettings(context: Context) {
            val intent = Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            runCatching { context.startActivity(intent) }
        }
    }

    private val seen = LinkedHashMap<String, Long>()

    override fun onListenerConnected() {
        super.onListenerConnected()
        connected = true
    }

    override fun onListenerDisconnected() {
        connected = false
        super.onListenerDisconnected()
    }

    override fun onNotificationPosted(sbn: StatusBarNotification?) {
        val notification = sbn?.notification ?: return
        val ctx = applicationContext ?: return
        if (sbn.packageName == ctx.packageName) return
        val flags = notification.flags
        if (flags and android.app.Notification.FLAG_GROUP_SUMMARY != 0) return
        if (flags and android.app.Notification.FLAG_ONGOING_EVENT != 0) return

        val extras = notification.extras ?: return
        if (extras.containsKey(android.app.Notification.EXTRA_MEDIA_SESSION)) return

        val title = extras.getCharSequence(android.app.Notification.EXTRA_TITLE)?.toString().orEmpty()
        val text = (extras.getCharSequence(android.app.Notification.EXTRA_BIG_TEXT)
            ?: extras.getCharSequence(android.app.Notification.EXTRA_TEXT))?.toString().orEmpty()
        if (title.isBlank() && text.isBlank()) return

        val id = sbn.key ?: sbn.packageName
        val postedAt = sbn.postTime / 1000
        val fingerprint = "$id|$postedAt|${title.hashCode()}|${text.hashCode()}"
        synchronized(seen) {
            if (seen.containsKey(fingerprint)) return
            seen[fingerprint] = postedAt
            if (seen.size > 200) {
                val oldest = seen.keys.firstOrNull()
                if (oldest != null) seen.remove(oldest)
            }
        }

        val contentIntent = notification.contentIntent
        if (contentIntent != null) {
            synchronized(recentIntents) {
                recentIntents[id] = contentIntent
                while (recentIntents.size > INTENT_CACHE_LIMIT) {
                    val oldest = recentIntents.keys.firstOrNull() ?: break
                    recentIntents.remove(oldest)
                }
            }
        }

        val appName = try {
            val info = ctx.packageManager.getApplicationInfo(sbn.packageName, 0)
            ctx.packageManager.getApplicationLabel(info).toString()
        } catch (_: Exception) {
            sbn.packageName
        }
        val safeTitle = TextUtils.substring(title, 0, minOf(title.length, 512))
        publishAsync(id, sbn.packageName, appName, safeTitle, text, postedAt)
    }

    private fun publishAsync(
        id: String,
        app: String,
        appName: String,
        title: String,
        text: String,
        postedAt: Long,
    ) {
        Thread {
            try {
                val error = NativeBridge.nativeNotifyPublish(id, app, appName, title, text, postedAt)
                if (error.isNotBlank()) Log.d(TAG, "通知未镜像: $error")
            } catch (e: Exception) {
                Log.w(TAG, "通知镜像失败", e)
            }
        }.start()
    }
}
