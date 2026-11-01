package com.linkbridge.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.Rect
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.provider.Settings
import android.view.GestureDetector
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.Toast
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * 悬浮球前台服务：通过 TYPE_APPLICATION_OVERLAY 覆盖在所有应用之上。
 * 支持位置（拖动吸边并记忆）、大小、颜色、透明度、点击/长按动作与显示规则；
 * 球体自动避开状态栏与底部手势区，避免遮挡核心操作区域。
 */
class FloatingBallService : Service() {
    companion object {
        private const val CHANNEL_ID = "linkbridge_floating"
        private const val NOTIFICATION_ID = 4200
        private const val ACTION_REFRESH = "com.linkbridge.app.FLOATING_BALL_REFRESH"

        @Volatile
        var isRunning: Boolean = false
            private set

        fun start(context: Context) {
            runCatching { context.startForegroundService(Intent(context, FloatingBallService::class.java)) }
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, FloatingBallService::class.java))
        }

        /** 参数变更后让正在运行的服务立即生效（未运行时无副作用）。 */
        fun refresh(context: Context) {
            if (!isRunning) return
            runCatching {
                context.startService(
                    Intent(context, FloatingBallService::class.java).setAction(ACTION_REFRESH),
                )
            }
        }

        fun canDrawOverlay(context: Context): Boolean = Settings.canDrawOverlays(context)

        fun openOverlaySettings(context: Context) {
            val intent = Intent(
                Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                Uri.parse("package:${context.packageName}"),
            ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            runCatching { context.startActivity(intent) }
        }
    }

    private lateinit var windowManager: WindowManager
    private val handler = Handler(Looper.getMainLooper())
    private var ball: BallView? = null
    private var params: WindowManager.LayoutParams? = null
    private var wheel: WheelView? = null
    private var settings = FloatingBallPrefs.Settings()
    private var attached = false

    private val visibilityTick = object : Runnable {
        override fun run() {
            applyVisibility()
            handler.postDelayed(this, 1000)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        LinkBridgeEngine.init(applicationContext)
        windowManager = getSystemService(Context.WINDOW_SERVICE) as WindowManager
        ensureChannel()
        isRunning = true
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForegroundCompat()
        settings = FloatingBallPrefs.load(this)
        if (!settings.enabled) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (!canDrawOverlay(this)) {
            toast("悬浮球需要「显示在其他应用上层」权限，请先授权")
            stopSelf()
            return START_NOT_STICKY
        }
        if (intent?.action == ACTION_REFRESH) {
            applySettings()
        } else {
            createOrUpdateBall()
        }
        return START_STICKY
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        closeWheel()
        removeBall()
        isRunning = false
        super.onDestroy()
    }

    // ---- 悬浮球视图 ----

    private fun createOrUpdateBall() {
        if (attached && ball != null) {
            applySettings()
            return
        }
        removeBall()
        val sizePx = dp(settings.sizeDp)
        val metrics = resources.displayMetrics
        val p = WindowManager.LayoutParams(
            sizePx,
            sizePx,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT,
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            val defaultX = metrics.widthPixels - sizePx - dp(12)
            x = if (settings.x >= 0) settings.x else defaultX
            y = if (settings.y >= 0) settings.y else metrics.heightPixels / 2
            clampPosition(metrics.widthPixels, metrics.heightPixels, sizePx)
        }
        val view = BallView(this, settings.color, settings.sizeDp)
        view.alpha = settings.alpha
        view.setOnTouchListener(DragTouchListener(view, p, sizePx))
        try {
            windowManager.addView(view, p)
            ball = view
            params = p
            attached = true
            handler.post(visibilityTick)
        } catch (e: Exception) {
            toast("悬浮球创建失败：${e.message ?: e.javaClass.simpleName}")
            stopSelf()
        }
    }

    private fun applySettings() {
        val view = ball ?: return
        val p = params ?: return
        settings = FloatingBallPrefs.load(this)
        if (!settings.enabled) {
            stopSelf()
            return
        }
        val sizePx = dp(settings.sizeDp)
        p.width = sizePx
        p.height = sizePx
        val metrics = resources.displayMetrics
        p.clampPosition(metrics.widthPixels, metrics.heightPixels, sizePx)
        view.applyStyle(settings.color, settings.sizeDp)
        view.alpha = settings.alpha
        runCatching { windowManager.updateViewLayout(view, p) }
        applyVisibility()
    }

    private fun applyVisibility() {
        val view = ball ?: return
        val visible = settings.enabled &&
            (settings.visibility == FloatingBallPrefs.VISIBILITY_ALWAYS || LinkBridgeEngine.running.value) &&
            !(ForegroundAppTracker.current?.let { it in settings.hiddenApps } ?: false)
        view.visibility = if (visible) View.VISIBLE else View.GONE
    }

    private fun removeBall() {
        handler.removeCallbacks(visibilityTick)
        val view = ball ?: return
        runCatching { windowManager.removeView(view) }
        ball = null
        attached = false
    }

    private fun WindowManager.LayoutParams.clampPosition(width: Int, height: Int, sizePx: Int) {
        val maxX = (width - sizePx).coerceAtLeast(0)
        // 底部预留 96dp：避开手势导航区与常见操作按钮
        val maxY = (height - sizePx - dp(96)).coerceAtLeast(dp(32))
        x = x.coerceIn(0, maxX)
        y = y.coerceIn(dp(32), maxY)
    }

    private inner class DragTouchListener(
        private val view: View,
        private val p: WindowManager.LayoutParams,
        private val sizePx: Int,
    ) : View.OnTouchListener {
        private val detector = GestureDetector(
            this@FloatingBallService,
            object : GestureDetector.SimpleOnGestureListener() {
                override fun onSingleTapConfirmed(e: MotionEvent): Boolean {
                    if (dragged) return false
                    performClickAction()
                    return true
                }

                override fun onDoubleTap(e: MotionEvent): Boolean {
                    if (dragged) return false
                    performDoubleClickAction()
                    return true
                }

                override fun onLongPress(e: MotionEvent) {
                    if (dragged) return
                    performLongPressAction()
                }
            },
        )
        private var downRawX = 0f
        private var downRawY = 0f
        private var startX = 0
        private var startY = 0
        private var dragged = false

        override fun onTouch(v: View, event: MotionEvent): Boolean {
            detector.onTouchEvent(event)
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    downRawX = event.rawX
                    downRawY = event.rawY
                    startX = p.x
                    startY = p.y
                    dragged = false
                }
                MotionEvent.ACTION_MOVE -> {
                    val dx = event.rawX - downRawX
                    val dy = event.rawY - downRawY
                    if (abs(dx) > dp(6) || abs(dy) > dp(6)) dragged = true
                    if (dragged) {
                        val metrics = resources.displayMetrics
                        p.x = startX + dx.toInt()
                        p.y = startY + dy.toInt()
                        p.clampPosition(metrics.widthPixels, metrics.heightPixels, sizePx)
                        runCatching { windowManager.updateViewLayout(v, p) }
                    }
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    if (dragged) {
                        snapToEdge(p, sizePx)
                        runCatching { windowManager.updateViewLayout(v, p) }
                        settings = FloatingBallPrefs.update(this@FloatingBallService) {
                            it.copy(x = p.x, y = p.y)
                        }
                    }
                    dragged = false
                }
            }
            return true
        }
    }

    private fun snapToEdge(p: WindowManager.LayoutParams, sizePx: Int) {
        val metrics = resources.displayMetrics
        val margin = dp(12)
        val left = margin
        val right = (metrics.widthPixels - sizePx - margin).coerceAtLeast(0)
        p.x = if (p.x + sizePx / 2 < metrics.widthPixels / 2) left else right
        p.clampPosition(metrics.widthPixels, metrics.heightPixels, sizePx)
    }

    // ---- 点击 / 长按动作 ----

    private fun performClickAction() {
        when (settings.clickAction) {
            FloatingBallPrefs.CLICK_WHEEL -> openWheel()
            FloatingBallPrefs.CLICK_OPEN_APP -> openMainApp()
            FloatingBallPrefs.CLICK_TOGGLE_SERVICE -> toggleSyncService()
        }
    }

    private fun performDoubleClickAction() {
        when (settings.doubleClickAction) {
            FloatingBallPrefs.DOUBLE_QUICK_CLIP -> quickClipboardSync()
        }
    }

    private fun performLongPressAction() {
        when (settings.longPressAction) {
            FloatingBallPrefs.LONG_OPEN_SETTINGS -> openMainApp()
            FloatingBallPrefs.LONG_TOGGLE_SERVICE -> toggleSyncService()
            FloatingBallPrefs.LONG_HIDE -> hideFloatingBall()
        }
    }

    private fun hideFloatingBall() {
        FloatingBallPrefs.update(this) { it.copy(enabled = false) }
        toast("悬浮球已隐藏，可在连桥设置中重新开启")
        stopSelf()
    }

    private fun openMainApp() {
        val intent = Intent(this, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        runCatching { startActivity(intent) }
    }

    private fun toggleSyncService() {
        if (LinkBridgeEngine.running.value) {
            LinkBridgeService.stop(this)
            toast("同步服务已停止")
        } else {
            LinkBridgeService.start(this)
            toast("同步服务已启动")
        }
        handler.postDelayed({ applyVisibility() }, 500)
    }

    /** 双击：短暂唤起前台窗口读取剪贴板并同步，完成后自动退出。 */
    private fun quickClipboardSync() {
        runCatching {
            startActivity(
                Intent(this, QuickClipActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION),
            )
        }.onFailure { toast("无法打开快捷同步窗口：${it.message}") }
    }

    // ---- 功能轮盘 ----

    private fun openWheel() {
        if (wheel != null) {
            closeWheel()
            return
        }
        val p = params ?: return
        val metrics = resources.displayMetrics
        // 轮盘整体需要完整落在屏幕内：靠近边缘时把圆心向内收，而不是让扇区被裁掉
        val wheelRadius = 118f * metrics.density
        val margin = 8f * metrics.density
        val minX = wheelRadius + margin
        val maxX = (metrics.widthPixels - wheelRadius - margin).coerceAtLeast(minX)
        val minY = wheelRadius + margin
        val maxY = (metrics.heightPixels - wheelRadius - margin).coerceAtLeast(minY)
        val cx = (p.x + p.width / 2f).coerceIn(minX, maxX).toInt()
        val cy = (p.y + p.height / 2f).coerceIn(minY, maxY).toInt()
        val items = listOf(
            WheelItem("同步剪贴板") { quickClipboardSync() },
            WheelItem("打开连桥") { openMainApp() },
            WheelItem(if (LinkBridgeEngine.running.value) "停止服务" else "启动服务") {
                toggleSyncService()
            },
            WheelItem("隐藏悬浮球") { hideFloatingBall() },
        )
        val lp = WindowManager.LayoutParams(
            metrics.widthPixels,
            metrics.heightPixels,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT,
        ).apply { gravity = Gravity.TOP or Gravity.START }
        val view = WheelView(this, cx, cy, settings.color, items) { closeWheel() }
        try {
            windowManager.addView(view, lp)
            wheel = view
        } catch (e: Exception) {
            toast("功能轮盘打开失败：${e.message ?: e.javaClass.simpleName}")
        }
    }

    private fun closeWheel() {
        val view = wheel ?: return
        wheel = null
        runCatching { windowManager.removeView(view) }
    }

    // ---- 前台通知 ----

    private fun ensureChannel() {
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (manager.getNotificationChannel(CHANNEL_ID) == null) {
            manager.createNotificationChannel(
                NotificationChannel(
                    CHANNEL_ID,
                    getString(R.string.channel_floating),
                    NotificationManager.IMPORTANCE_LOW,
                ),
            )
        }
    }

    private fun startForegroundCompat() {
        val builder = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(getString(R.string.floating_running))
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .setOngoing(true)
        val notification: Notification = builder.build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE,
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    private fun toast(text: String) {
        Toast.makeText(this, text, Toast.LENGTH_SHORT).show()
    }

    /** 自绘圆形悬浮球：纯色圆 + 描边 + 居中「桥」字，避免引入图片资源。 */
    private class BallView(context: Context, color: Int, sizeDp: Int) : View(context) {
        private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
        private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
        private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            this.color = Color.WHITE
            textAlign = Paint.Align.CENTER
            isFakeBoldText = true
        }
        private var sizeDp = sizeDp

        init {
            applyStyle(color, sizeDp)
        }

        fun applyStyle(color: Int, sizeDp: Int) {
            this.sizeDp = sizeDp
            val density = resources.displayMetrics.density
            fill.color = color
            ring.color = Color.argb(180, 255, 255, 255)
            ring.strokeWidth = 2f * density
            label.textSize = (sizeDp * 0.42f) * density
            invalidate()
        }

        override fun onDraw(canvas: Canvas) {
            val cx = width / 2f
            val cy = height / 2f
            val radius = min(width, height) / 2f - ring.strokeWidth
            canvas.drawCircle(cx, cy, radius, fill)
            canvas.drawCircle(cx, cy, radius, ring)
            val bounds = Rect()
            label.getTextBounds("桥", 0, 1, bounds)
            canvas.drawText("桥", cx, cy - (bounds.top + bounds.bottom) / 2f, label)
        }
    }
}

/** 当前前台应用包名（由无障碍服务更新）；未启用无障碍时保持 null。 */
object ForegroundAppTracker {
    @Volatile
    var current: String? = null
}

/** 轮盘中的一个扇区动作。 */
private class WheelItem(val label: String, val action: () -> Unit)

/**
 * 全屏半透明的功能轮盘：以悬浮球为圆心绘制扇形按钮，点击扇区执行对应动作；
 * 点击中心圆或轮盘外空白处关闭。绘制与命中判定全部本地完成，不依赖 popup 窗口。
 */
private class WheelView(
    context: Context,
    private val centerX: Int,
    private val centerY: Int,
    private val baseColor: Int,
    private val items: List<WheelItem>,
    private val onDismiss: () -> Unit,
) : View(context) {
    private val density = resources.displayMetrics.density
    private val outerRadius = 118f * density
    private val innerRadius = 46f * density
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = Color.argb(200, 255, 255, 255)
        strokeWidth = 1.5f * density
    }
    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textAlign = Paint.Align.CENTER
        textSize = 13f * density
        isFakeBoldText = true
    }
    private var dismissed = false

    init {
        setBackgroundColor(Color.argb(70, 0, 0, 0))
    }

    override fun onDraw(canvas: Canvas) {
        val n = items.size
        if (n == 0) return
        val cx = centerX.toFloat()
        val cy = centerY.toFloat()
        val sweep = 360f / n
        val red = Color.red(baseColor)
        val green = Color.green(baseColor)
        val blue = Color.blue(baseColor)

        fill.color = baseColor
        canvas.drawCircle(cx, cy, outerRadius, fill)
        canvas.drawCircle(cx, cy, outerRadius, ring)

        for (i in 0 until n) {
            val start = -90f + i * sweep
            fill.color = Color.argb(if (i % 2 == 0) 235 else 205, red, green, blue)
            canvas.drawArc(
                cx - outerRadius, cy - outerRadius,
                cx + outerRadius, cy + outerRadius,
                start, sweep, true, fill,
            )
            val mid = Math.toRadians((start + sweep / 2).toDouble())
            val radius = (outerRadius + innerRadius) / 2f
            val tx = cx + (radius * cos(mid)).toFloat()
            val ty = cy + (radius * sin(mid)).toFloat()
            val bounds = Rect()
            labelPaint.getTextBounds(items[i].label, 0, items[i].label.length, bounds)
            canvas.drawText(items[i].label, tx, ty - (bounds.top + bounds.bottom) / 2f, labelPaint)
        }

        fill.color = baseColor
        canvas.drawCircle(cx, cy, innerRadius, fill)
        canvas.drawCircle(cx, cy, innerRadius, ring)
        val bounds = Rect()
        labelPaint.getTextBounds("关闭", 0, 2, bounds)
        canvas.drawText("关闭", cx, cy - (bounds.top + bounds.bottom) / 2f, labelPaint)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.actionMasked != MotionEvent.ACTION_UP) return true
        if (dismissed) return true
        val dx = (event.x - centerX).toDouble()
        val dy = (event.y - centerY).toDouble()
        val distance = sqrt(dx * dx + dy * dy).toFloat()
        val index = if (distance in innerRadius..(outerRadius + 12f * density) && items.isNotEmpty()) {
            val degrees = Math.toDegrees(atan2(dy, dx)) + 90.0
            val normalized = ((degrees % 360 + 360) % 360)
            (normalized / (360.0 / items.size)).toInt().coerceIn(0, items.size - 1)
        } else {
            -1
        }
        dismissed = true
        onDismiss()
        if (index >= 0) items[index].action()
        return true
    }
}

