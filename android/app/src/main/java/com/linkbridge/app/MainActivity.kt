package com.linkbridge.app

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.json.JSONObject

class MainActivity : ComponentActivity() {
    private var accessibilityEnabled by mutableStateOf(false)
    private var refreshTick by mutableIntStateOf(0)

    private val projectionLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            val data = result.data
            if (result.resultCode == Activity.RESULT_OK && data != null) {
                ScreenShareService.start(this, result.resultCode, data)
            } else {
                LinkBridgeEngine.setMessage("屏幕共享未授权")
            }
        }

    private val notificationPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        LinkBridgeEngine.init(applicationContext)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        }

        setContent {
            MaterialTheme {
                LinkBridgeScreen(
                    accessibilityEnabled = accessibilityEnabled,
                    refreshTick = refreshTick,
                    onShareScreen = ::requestScreenShare,
                    onOpenAccessibility = { InputBridge.openSettings(this) },
                    onRefreshAccessibility = { accessibilityEnabled = InputBridge.isEnabled(this) },
                )
            }
        }
    }

    override fun onResume() {
        super.onResume()
        accessibilityEnabled = InputBridge.isEnabled(this)
        refreshTick += 1
        // 剪贴板自动获取：回到前台时读取当前剪贴板，内容变化才推送
        if (ClipboardAutoSync.isAutoEnabled(this) && LinkBridgeEngine.running.value) {
            ClipboardAutoSync.sync(this, force = false) { LinkBridgeEngine.setMessage(it) }
        }
    }

    private fun requestScreenShare() {
        val manager = getSystemService(MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        projectionLauncher.launch(manager.createScreenCaptureIntent())
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LinkBridgeScreen(
    accessibilityEnabled: Boolean,
    refreshTick: Int,
    onShareScreen: () -> Unit,
    onOpenAccessibility: () -> Unit,
    onRefreshAccessibility: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    val running by LinkBridgeEngine.running.collectAsState()
    val status by LinkBridgeEngine.status.collectAsState()
    val devices by LinkBridgeEngine.devices.collectAsState()
    val conflicts by LinkBridgeEngine.conflicts.collectAsState()
    val catalog by LinkBridgeEngine.permissionCatalog.collectAsState()
    val message by LinkBridgeEngine.message.collectAsState()
    val confirm by LinkBridgeEngine.confirmRequest.collectAsState()

    var ip by remember { mutableStateOf("") }
    var code by remember { mutableStateOf("") }
    var pairCode by remember { mutableStateOf("") }
    var pairing by remember { mutableStateOf(false) }
    var pendingRemove by remember { mutableStateOf<JSONObject?>(null) }
    var autoClip by remember { mutableStateOf(ClipboardAutoSync.isAutoEnabled(context)) }

    var ballSettings by remember { mutableStateOf(FloatingBallPrefs.load(context)) }
    var ballSize by remember(ballSettings.sizeDp) { mutableFloatStateOf(ballSettings.sizeDp.toFloat()) }
    var ballAlpha by remember(ballSettings.alpha) { mutableFloatStateOf(ballSettings.alpha) }
    var hiddenInput by remember { mutableStateOf("") }
    val overlayAllowed = remember(refreshTick) { FloatingBallService.canDrawOverlay(context) }
    val notifyAccess = remember(refreshTick) { LinkBridgeNotificationListener.isEnabled(context) }
    val shizukuState = remember(refreshTick) { ShizukuBridge.state(context) }
    val notifyCfg = status.optJSONObject("notify")
    val notifyRuntimeEnabled = notifyCfg?.optBoolean("runtime_enabled", true) ?: true

    fun commitBall(next: FloatingBallPrefs.Settings) {
        ballSettings = next
        FloatingBallPrefs.save(context, next)
        if (!next.enabled) {
            FloatingBallService.stop(context)
            return
        }
        if (!FloatingBallService.canDrawOverlay(context)) {
            FloatingBallService.openOverlaySettings(context)
            LinkBridgeEngine.setMessage("请先授予「显示在其他应用上层」权限，再开启悬浮球")
            return
        }
        FloatingBallService.start(context)
        FloatingBallService.refresh(context)
    }

    Scaffold(
        topBar = { TopAppBar(title = { Text("连桥 LinkBridge") }) },
    ) { padding ->
        Column(
            modifier = Modifier
                .padding(padding)
                .padding(16.dp)
                .fillMaxWidth()
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(
                if (running) "服务运行中" else "服务已停止",
                style = MaterialTheme.typography.titleMedium,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = { LinkBridgeService.start(context) }) { Text("启动服务") }
                OutlinedButton(onClick = { LinkBridgeService.stop(context) }) { Text("停止服务") }
                OutlinedButton(onClick = {
                    scope.launch { LinkBridgeEngine.refresh() }
                }) { Text("刷新") }
            }

            // 关键反馈放在页面顶部，避免长页面里结果被滚出屏幕
            if (message.isNotBlank()) {
                Card(Modifier.fillMaxWidth()) {
                    Text(
                        message,
                        modifier = Modifier.padding(12.dp),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }

            Text("设备: ${status.optString("name", "-")}", style = MaterialTheme.typography.bodyMedium)
            Text("设备 ID: ${status.optString("device_id", "-")}", style = MaterialTheme.typography.bodySmall)
            Text("指纹: ${status.optString("fingerprint", "-")}", style = MaterialTheme.typography.bodySmall)
            val sync = status.optJSONObject("sync")
            if (sync != null) {
                Text(
                    "同步: ${if (sync.optBoolean("enabled")) "启用" else "停用"}  " +
                        "目录: ${sync.optString("folder", "-")}",
                    style = MaterialTheme.typography.bodySmall,
                )
                Text(
                    "索引: ${sync.optInt("index")}  未解决冲突: ${sync.optInt("conflicts")}  " +
                        "心跳: ${status.optInt("heartbeat_interval")}s",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            Text("配对端口 ${status.optInt("pair_port")}  传输 ${status.optInt("transfer_port")}  " +
                "剪贴板 ${status.optInt("clipboard_port")}  屏幕 ${status.optInt("screen_port")}  " +
                "同步 ${status.optInt("sync_port")}", style = MaterialTheme.typography.bodySmall)

            Text("配对", style = MaterialTheme.typography.titleMedium)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = ip,
                    onValueChange = { ip = it },
                    label = { Text("对端 IP") },
                    modifier = Modifier.weight(1f),
                    singleLine = true,
                )
                OutlinedTextField(
                    value = code,
                    onValueChange = { code = it },
                    label = { Text("配对码") },
                    modifier = Modifier.width(120.dp),
                    singleLine = true,
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = {
                    scope.launch {
                        val result = LinkBridgeEngine.requestPairCode()
                        if (result.has("error")) {
                            LinkBridgeEngine.setMessage(result.optString("error"))
                        } else {
                            pairCode = result.optString("code")
                        }
                    }
                }) { Text("生成配对码") }
                Button(
                    enabled = !pairing,
                    onClick = {
                        if (ip.isBlank() || code.isBlank()) {
                            LinkBridgeEngine.setMessage("请先填写对端 IP 和配对码")
                            return@Button
                        }
                        scope.launch {
                            pairing = true
                            try {
                                LinkBridgeEngine.setMessage("正在连接 ${ip.trim()} …")
                                if (!ensureServiceRunning(context)) {
                                    LinkBridgeEngine.setMessage("服务未启动，未能开始配对；请先点击「启动服务」后重试")
                                    return@launch
                                }
                                val error = LinkBridgeEngine.pair(ip, code)
                                LinkBridgeEngine.setMessage(
                                    if (error.isBlank()) "配对成功"
                                    else "配对失败: " + pairErrorText(error)
                                )
                            } finally {
                                pairing = false
                            }
                        }
                    },
                ) { Text(if (pairing) "配对中…" else "向对端配对") }
            }
            if (pairCode.isNotBlank()) {
                Card(Modifier.fillMaxWidth()) {
                    Text(
                        "本机配对码: $pairCode（5 分钟内有效，单次使用）",
                        modifier = Modifier.padding(12.dp),
                        style = MaterialTheme.typography.titleMedium,
                    )
                }
            }

            Text("受信任设备", style = MaterialTheme.typography.titleMedium)
            if (devices.isEmpty()) {
                Text("尚未配对设备", style = MaterialTheme.typography.bodySmall)
            }
            devices.forEach { device ->
                Card(Modifier.fillMaxWidth()) {
                    Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text("${device.optString("name")}  (${device.optString("platform")})")
                        Text(
                            "${device.optString("device_id")}  ${device.optString("presence")}  " +
                                device.optString("ip", "-"),
                            style = MaterialTheme.typography.bodySmall,
                        )
                        Text(
                            "权限掩码 ${device.optString("permissions_text", "-")}",
                            style = MaterialTheme.typography.bodySmall,
                        )
                        val mask = device.optLong("permissions")
                        catalog.forEach { perm ->
                            val flag = perm.optLong("flag")
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Column(Modifier.weight(1f)) {
                                    Text(perm.optString("name"), style = MaterialTheme.typography.bodyMedium)
                                    Text(
                                        perm.optString("description"),
                                        style = MaterialTheme.typography.bodySmall,
                                    )
                                }
                                Switch(
                                    checked = (mask and flag) != 0L,
                                    onCheckedChange = { on ->
                                        val newMask = if (on) mask or flag else mask and flag.inv()
                                        scope.launch {
                                            val error = LinkBridgeEngine.setPermissions(
                                                device.optString("device_id"),
                                                newMask,
                                            )
                                            if (error.isNotBlank()) {
                                                LinkBridgeEngine.setMessage("权限变更失败: $error")
                                            } else {
                                                LinkBridgeEngine.setMessage("权限已更新并生效")
                                            }
                                        }
                                    },
                                )
                            }
                        }
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            TextButton(onClick = {
                                scope.launch {
                                    val error = LinkBridgeEngine.syncNow(device.optString("device_id"))
                                    LinkBridgeEngine.setMessage(
                                        if (error.isBlank()) "同步完成" else "同步失败: $error",
                                    )
                                }
                            }) { Text("立即同步") }
                            TextButton(
                                onClick = { pendingRemove = device },
                                colors = ButtonDefaults.textButtonColors(
                                    contentColor = MaterialTheme.colorScheme.error,
                                ),
                            ) { Text("移除设备") }
                        }
                    }
                }
            }

            Text("剪贴板（自动获取）", style = MaterialTheme.typography.titleMedium)
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("自动获取并同步", style = MaterialTheme.typography.bodyMedium)
                    Text(
                        "回到前台时自动读取当前剪贴板，内容变化才推送",
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
                Switch(
                    checked = autoClip,
                    onCheckedChange = {
                        autoClip = it
                        ClipboardAutoSync.setAutoEnabled(context, it)
                    },
                )
            }
            val clipPreview = remember(refreshTick) { ClipboardAutoSync.preview(context) }
            Text(
                "当前剪贴板: ${clipPreview ?: "空或不可读（请保持本页在前台）"}",
                style = MaterialTheme.typography.bodySmall,
            )
            OutlinedButton(onClick = {
                ClipboardAutoSync.sync(context, force = true) { LinkBridgeEngine.setMessage(it) }
            }) { Text("立即同步当前剪贴板") }
            Text(
                "说明：双击悬浮球会短暂弹出前台窗口，自动获取剪贴板 → 同步到已授权设备 → 自动退出；" +
                    "Android 10+ 限制后台读取剪贴板，读取失败时会明确提示。",
                style = MaterialTheme.typography.bodySmall,
            )

            Text("屏幕共享（只读）", style = MaterialTheme.typography.titleMedium)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = onShareScreen) { Text("允许查看本机屏幕") }
                OutlinedButton(onClick = { ScreenShareService.stop(context) }) { Text("停止共享") }
            }

            Text("屏幕控制（无障碍）", style = MaterialTheme.typography.titleMedium)
            Text(
                if (accessibilityEnabled)
                    "无障碍服务已启用：已配对设备在每次会话确认后可注入触摸与按键。"
                else
                    "无障碍服务未启用：其他设备暂时无法控制本机屏幕。",
                style = MaterialTheme.typography.bodyMedium,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = onOpenAccessibility) { Text("打开无障碍设置") }
                OutlinedButton(onClick = onRefreshAccessibility) { Text("刷新状态") }
            }
            Text(
                "启用步骤：设置 → 无障碍 → 已下载的应用 → 连桥 LinkBridge → 打开。" +
                    "本服务只用于输入注入（手势与返回/主页/最近任务），不读取屏幕内容或文本。",
                style = MaterialTheme.typography.bodySmall,
            )

            Text("通知镜像（跨设备通知流转）", style = MaterialTheme.typography.titleMedium)
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("同步本机通知到已授权设备", modifier = Modifier.weight(1f))
                Switch(
                    checked = notifyRuntimeEnabled,
                    onCheckedChange = { LinkBridgeEngine.setNotifyEnabled(it) },
                )
            }
            Text(
                "通知使用权: ${if (notifyAccess) "已授予" else "未授予"}；" +
                    "Shizuku: ${ShizukuBridge.statusText(context)}",
                style = MaterialTheme.typography.bodySmall,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { LinkBridgeNotificationListener.openSettings(context) }) {
                    Text("通知使用权设置")
                }
                OutlinedButton(onClick = { ShizukuBridge.openShizuku(context) }) { Text("打开 Shizuku") }
                OutlinedButton(onClick = {
                    val ok = ShizukuBridge.requestPermission()
                    LinkBridgeEngine.setMessage(
                        if (ok) "已向 Shizuku 申请授权" else "请在 Shizuku 应用内手动授权（当前安装包未集成 API）",
                    )
                }) { Text("申请授权") }
            }
            Text(
                "使用步骤：1) 在系统设置中授予连桥「通知使用权」；2) 在设备权限中为对端开启「通知镜像」；" +
                    "3) 点击镜像通知会回到来源设备执行预设动作；4) 需要自动点击时建议启用 Shizuku，" +
                    "不可用时自动降级为无障碍注入（需开启无障碍服务）。",
                style = MaterialTheme.typography.bodySmall,
            )

            Text("悬浮球", style = MaterialTheme.typography.titleMedium)
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("显示悬浮球", modifier = Modifier.weight(1f))
                Switch(
                    checked = ballSettings.enabled,
                    onCheckedChange = { on -> commitBall(ballSettings.copy(enabled = on, x = -1, y = -1)) },
                )
            }
            Text(
                "悬浮层权限: ${if (overlayAllowed) "已授予" else "未授予"}；显示规则: " +
                    if (ballSettings.visibility == FloatingBallPrefs.VISIBILITY_ALWAYS) "始终显示" else "仅同步服务运行时",
                style = MaterialTheme.typography.bodySmall,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                if (!overlayAllowed) {
                    OutlinedButton(onClick = { FloatingBallService.openOverlaySettings(context) }) {
                        Text("授予悬浮层权限")
                    }
                }
                OutlinedButton(onClick = { commitBall(ballSettings.copy(enabled = true)) }) {
                    Text("启动悬浮球")
                }
                OutlinedButton(onClick = { commitBall(ballSettings.copy(enabled = false)) }) {
                    Text("停止")
                }
                OutlinedButton(onClick = { commitBall(ballSettings.copy(x = -1, y = -1)) }) {
                    Text("复位位置")
                }
            }
            Text("大小: ${ballSize.toInt()} dp", style = MaterialTheme.typography.bodySmall)
            Slider(
                value = ballSize,
                onValueChange = { ballSize = it },
                valueRange = 40f..88f,
                onValueChangeFinished = { commitBall(ballSettings.copy(sizeDp = ballSize.toInt())) },
            )
            Text("透明度: ${(ballAlpha * 100).toInt()}%", style = MaterialTheme.typography.bodySmall)
            Slider(
                value = ballAlpha,
                onValueChange = { ballAlpha = it },
                valueRange = 0.2f..1f,
                onValueChangeFinished = { commitBall(ballSettings.copy(alpha = ballAlpha)) },
            )
            Text("颜色", style = MaterialTheme.typography.bodySmall)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                BALL_COLORS.forEach { (color, name) ->
                    Box(
                        modifier = Modifier
                            .size(38.dp)
                            .background(Color(color), CircleShape)
                            .clickable { commitBall(ballSettings.copy(color = color)) },
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            name,
                            color = Color.White,
                            style = MaterialTheme.typography.labelSmall,
                        )
                    }
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("单击动作", modifier = Modifier.weight(1f))
                OutlinedButton(onClick = {
                    commitBall(ballSettings.copy(clickAction = nextClickAction(ballSettings.clickAction)))
                }) { Text(clickActionLabel(ballSettings.clickAction)) }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("双击动作", modifier = Modifier.weight(1f))
                OutlinedButton(onClick = {
                    val next = if (ballSettings.doubleClickAction == FloatingBallPrefs.DOUBLE_QUICK_CLIP)
                        FloatingBallPrefs.DOUBLE_NONE else FloatingBallPrefs.DOUBLE_QUICK_CLIP
                    commitBall(ballSettings.copy(doubleClickAction = next))
                }) {
                    Text(
                        if (ballSettings.doubleClickAction == FloatingBallPrefs.DOUBLE_QUICK_CLIP)
                            "快捷同步剪贴板" else "无动作",
                    )
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("长按动作", modifier = Modifier.weight(1f))
                OutlinedButton(onClick = {
                    commitBall(ballSettings.copy(longPressAction = nextLongAction(ballSettings.longPressAction)))
                }) { Text(longActionLabel(ballSettings.longPressAction)) }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("显示规则", modifier = Modifier.weight(1f))
                OutlinedButton(onClick = {
                    val next = if (ballSettings.visibility == FloatingBallPrefs.VISIBILITY_ALWAYS)
                        FloatingBallPrefs.VISIBILITY_RUNNING else FloatingBallPrefs.VISIBILITY_ALWAYS
                    commitBall(ballSettings.copy(visibility = next))
                }) {
                    Text(
                        if (ballSettings.visibility == FloatingBallPrefs.VISIBILITY_ALWAYS)
                            "始终显示" else "仅服务运行时",
                    )
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(
                    value = hiddenInput,
                    onValueChange = { hiddenInput = it },
                    label = { Text("在以下应用隐藏（包名）") },
                    modifier = Modifier.weight(1f),
                    singleLine = true,
                )
                OutlinedButton(onClick = {
                    val pkg = hiddenInput.trim()
                    if (pkg.isNotBlank()) {
                        commitBall(ballSettings.copy(hiddenApps = ballSettings.hiddenApps + pkg))
                    }
                    hiddenInput = ""
                }) { Text("添加") }
            }
            if (ballSettings.hiddenApps.isNotEmpty()) {
                Text(
                    "隐藏应用: ${ballSettings.hiddenApps.joinToString()}",
                    style = MaterialTheme.typography.bodySmall,
                )
                TextButton(onClick = { commitBall(ballSettings.copy(hiddenApps = emptySet())) }) {
                    Text("清空隐藏列表")
                }
            }

            if (conflicts.isNotEmpty()) {
                Text("未解决冲突", style = MaterialTheme.typography.titleMedium)
                conflicts.forEach { conflict ->
                    val path = conflict.optString("path")
                    Card(Modifier.fillMaxWidth()) {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text(path)
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                TextButton(onClick = {
                                    scope.launch { LinkBridgeEngine.setMessage(resolve(path, "keep_local")) }
                                }) { Text("保留本端") }
                                TextButton(onClick = {
                                    scope.launch { LinkBridgeEngine.setMessage(resolve(path, "keep_remote")) }
                                }) { Text("保留对端") }
                                TextButton(onClick = {
                                    scope.launch { LinkBridgeEngine.setMessage(resolve(path, "keep_both")) }
                                }) { Text("两者保留") }
                            }
                        }
                    }
                }
            }

        }
    }

    pendingRemove?.let { device ->
        AlertDialog(
            onDismissRequest = { pendingRemove = null },
            title = { Text("移除设备") },
            text = {
                Text(
                    "确认移除「${device.optString("name")}」？\n" +
                        "移除后需重新配对才能继续同步、互传文件与共享剪贴板。",
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    val id = device.optString("device_id")
                    pendingRemove = null
                    scope.launch {
                        val error = LinkBridgeEngine.removeDevice(id)
                        LinkBridgeEngine.setMessage(
                            if (error.isBlank()) "已移除设备: $id" else "移除失败: $error",
                        )
                    }
                }) { Text("移除") }
            },
            dismissButton = {
                TextButton(onClick = { pendingRemove = null }) { Text("取消") }
            },
        )
    }

    confirm?.let { request ->
        AlertDialog(
            onDismissRequest = { LinkBridgeEngine.respondConfirm(request.optInt("id"), false) },
            title = { Text("来自 ${request.optString("name")} 的请求") },
            text = {
                Text(
                    when (request.optString("kind")) {
                        "pairing" -> "配对请求\n指纹: ${request.optString("fingerprint")}"
                        "clipboard" -> "剪贴板内容（${request.optString("detail")}）\n预览: ${request.optString("fingerprint")}"
                        "screen" -> if (request.optString("detail") == "control")
                            "对方请求控制你的屏幕（可注入触摸与按键）"
                        else "对方请求只读查看你的屏幕"
                        "notify_action" -> "对端请求在本机执行通知预设动作\n${request.optString("detail")}"
                        else -> request.optString("detail")
                    },
                )
            },
            confirmButton = {
                TextButton(onClick = { LinkBridgeEngine.respondConfirm(request.optInt("id"), true) }) {
                    Text("接受")
                }
            },
            dismissButton = {
                TextButton(onClick = { LinkBridgeEngine.respondConfirm(request.optInt("id"), false) }) {
                    Text("拒绝")
                }
            },
        )
    }
}

/** 冷启动时前台服务默认是停止状态；配对前先确保服务已启动。 */
private suspend fun ensureServiceRunning(context: Context, timeoutMs: Long = 4000): Boolean {
    if (LinkBridgeEngine.running.value) return true
    LinkBridgeService.start(context)
    var waited = 0L
    while (!LinkBridgeEngine.running.value && waited < timeoutMs) {
        delay(200)
        LinkBridgeEngine.refresh()
        waited += 200
    }
    return LinkBridgeEngine.running.value
}

private fun pairErrorText(error: String): String = when {
    error.contains("连接超时") || error.contains("连接对端失败") ->
        "$error（请确认对端已打开连桥并生成配对码、与本机在同一网络，且防火墙放行 45710 端口）"
    else -> error
}

private suspend fun resolve(path: String, action: String): String {
    val error = LinkBridgeEngine.resolveConflict(path, action)
    return if (error.isBlank()) "冲突已解决: $path ($action)" else "解决失败: $error"
}

private val BALL_COLORS = listOf(
    0xFF3F51B5.toInt() to "蓝",
    0xFF00897B.toInt() to "青",
    0xFFEF6C00.toInt() to "橙",
    0xFFC62828.toInt() to "红",
    0xFF6A1B9A.toInt() to "紫",
)

private fun nextClickAction(current: String): String = when (current) {
    FloatingBallPrefs.CLICK_WHEEL -> FloatingBallPrefs.CLICK_OPEN_APP
    FloatingBallPrefs.CLICK_OPEN_APP -> FloatingBallPrefs.CLICK_TOGGLE_SERVICE
    FloatingBallPrefs.CLICK_TOGGLE_SERVICE -> FloatingBallPrefs.CLICK_NONE
    else -> FloatingBallPrefs.CLICK_WHEEL
}

private fun clickActionLabel(value: String): String = when (value) {
    FloatingBallPrefs.CLICK_OPEN_APP -> "打开连桥"
    FloatingBallPrefs.CLICK_TOGGLE_SERVICE -> "启停同步服务"
    FloatingBallPrefs.CLICK_NONE -> "无动作"
    else -> "打开功能轮盘"
}

private fun nextLongAction(current: String): String = when (current) {
    FloatingBallPrefs.LONG_TOGGLE_SERVICE -> FloatingBallPrefs.LONG_HIDE
    FloatingBallPrefs.LONG_HIDE -> FloatingBallPrefs.LONG_OPEN_SETTINGS
    else -> FloatingBallPrefs.LONG_TOGGLE_SERVICE
}

private fun longActionLabel(value: String): String = when (value) {
    FloatingBallPrefs.LONG_HIDE -> "隐藏悬浮球"
    FloatingBallPrefs.LONG_OPEN_SETTINGS -> "打开连桥设置"
    else -> "启停同步服务"
}

