package com.linkbridge.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.widget.Toast

/**
 * 通知镜像桥（C++ 侧经 JNI 调用）。
 * - showMirror：把来源设备（B）的通知在本机（A）以系统通知展示，点击后回到 B 执行预设动作；
 * - performAction：在来源设备侧执行动作（打开应用 / 预设坐标点击）。
 * 全流程异常都吞掉并返回 false，避免把异常抛回 native 线程导致通道中断。
 */
object NotifyBridge {
    private const val CHANNEL_ID = "linkbridge_mirror"
    private const val CHANNEL_NAME = "连桥通知镜像"

    private var appContext: Context? = null

    @JvmStatic
    fun init(context: Context) {
        appContext = context.applicationContext
        ensureChannel(context.applicationContext)
    }

    private fun context(): Context? = appContext ?: LinkBridgeApp.requireContext()

    @JvmStatic
    fun isReady(): Boolean {
        val ctx = context() ?: return false
        return notificationManager(ctx).areNotificationsEnabled()
    }

    @JvmStatic
    fun unavailableReason(): String {
        val ctx = context()
            ?: return "应用上下文未初始化（请重新打开连桥）"
        return if (!notificationManager(ctx).areNotificationsEnabled())
            "系统通知已被关闭，无法展示镜像通知（请在系统设置中允许连桥发送通知）"
        else ""
    }

    @JvmStatic
    fun showMirror(
        sourceDeviceId: String,
        sourceName: String,
        id: String,
        app: String,
        appName: String,
        title: String,
        text: String,
        postedAt: Long,
    ): Boolean {
        val ctx = context() ?: return false
        if (!isReady()) return false
        return try {
            val manager = notificationManager(ctx)
            val notificationId = ("$sourceDeviceId|$id").hashCode()
            val intent = Intent(ctx, MirrorActionReceiver::class.java).apply {
                putExtra(MirrorActionReceiver.EXTRA_SOURCE, sourceDeviceId)
                putExtra(MirrorActionReceiver.EXTRA_NOTIFICATION_ID, id)
                putExtra(MirrorActionReceiver.EXTRA_APP, app)
                putExtra(MirrorActionReceiver.EXTRA_APP_NAME, appName)
                putExtra(MirrorActionReceiver.EXTRA_TITLE, title)
                putExtra(MirrorActionReceiver.EXTRA_NOTIFY_ID, notificationId)
            }
            val pending = PendingIntent.getBroadcast(
                ctx,
                notificationId,
                intent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
            )
            val shownTitle = if (title.isBlank()) appName.ifBlank { app } else title
            val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                Notification.Builder(ctx, CHANNEL_ID)
            } else {
                Notification.Builder(ctx)
            }
            val notification = builder
                .setContentTitle(shownTitle)
                .setContentText(text)
                .setSubText("来自 ${sourceName.ifBlank { "另一台设备" }} · ${appName.ifBlank { app }}")
                .setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentIntent(pending)
                .setAutoCancel(true)
                .setWhen(if (postedAt > 0) postedAt * 1000 else System.currentTimeMillis())
                .setShowWhen(true)
                .build()
            manager.notify(notificationId, notification)
            true
        } catch (e: Exception) {
            toast("镜像通知展示失败：${e.message ?: e.javaClass.simpleName}")
            false
        }
    }

    @JvmStatic
    fun performAction(
        id: String,
        app: String,
        appName: String,
        title: String,
        mode: Int,
        x: Int,
        y: Int,
    ): Boolean {
        val ctx = context() ?: return false
        return try {
            // 先在来源设备唤起原始通知（走通知自带的 contentIntent），失败再降级打开应用。
            val originalOpened = LinkBridgeNotificationListener.fireOriginalIntent(id)
            when (mode) {
                0 -> originalOpened || openApp(ctx, app)
                1 -> {
                    if (originalOpened) Thread.sleep(500)
                    tap(ctx, x, y)
                }
                else -> originalOpened
            }
        } catch (e: Exception) {
            toast("通知动作执行失败：${e.message ?: e.javaClass.simpleName}")
            false
        }
    }

    /** mode=0：打开来源应用（通知所属应用）。 */
    private fun openApp(ctx: Context, pkg: String): Boolean {
        if (pkg.isBlank()) return false
        val launch = ctx.packageManager.getLaunchIntentForPackage(pkg) ?: return false
        launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        ctx.startActivity(launch)
        return true
    }

    /** mode=1：按来源设备屏幕坐标点击；优先 Shizuku，其次无障碍注入。 */
    private fun tap(ctx: Context, x: Int, y: Int): Boolean {
        if (x < 0 || y < 0) return false
        if (ShizukuBridge.tap(x, y)) return true
        if (InputBridge.inject(1, x, y, x, y, 0)) return true
        toast("无法执行点击：Shizuku 未就绪且无障碍服务未启用")
        return false
    }

    private fun notificationManager(ctx: Context): NotificationManager =
        ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

    private fun ensureChannel(ctx: Context) {
        val manager = notificationManager(ctx)
        if (manager.getNotificationChannel(CHANNEL_ID) == null) {
            manager.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, CHANNEL_NAME, NotificationManager.IMPORTANCE_DEFAULT)
                    .apply { description = "其他设备镜像到本机的通知" },
            )
        }
    }

    private fun toast(text: String) {
        Handler(Looper.getMainLooper()).post {
            Toast.makeText(context(), text, Toast.LENGTH_SHORT).show()
        }
    }
}
