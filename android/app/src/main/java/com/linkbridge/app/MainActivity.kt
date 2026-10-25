package com.linkbridge.app

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import kotlinx.coroutines.launch
import org.json.JSONObject

class MainActivity : ComponentActivity() {
    private var accessibilityEnabled by mutableStateOf(false)

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
    val message by LinkBridgeEngine.message.collectAsState()
    val confirm by LinkBridgeEngine.confirmRequest.collectAsState()

    var ip by remember { mutableStateOf("") }
    var code by remember { mutableStateOf("") }
    var pairCode by remember { mutableStateOf("") }
    var clipboard by remember { mutableStateOf("") }

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
                Button(onClick = {
                    scope.launch {
                        val error = LinkBridgeEngine.pair(ip, code)
                        LinkBridgeEngine.setMessage(if (error.isBlank()) "配对成功" else "配对失败: $error")
                    }
                }) { Text("向对端配对") }
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
                        TextButton(onClick = {
                            scope.launch {
                                val error = LinkBridgeEngine.syncNow(device.optString("device_id"))
                                LinkBridgeEngine.setMessage(
                                    if (error.isBlank()) "同步完成" else "同步失败: $error",
                                )
                            }
                        }) { Text("立即同步") }
                    }
                }
            }

            Text("剪贴板", style = MaterialTheme.typography.titleMedium)
            OutlinedTextField(
                value = clipboard,
                onValueChange = { clipboard = it },
                label = { Text("要推送的文本") },
                modifier = Modifier.fillMaxWidth(),
            )
            Button(onClick = {
                scope.launch {
                    val error = LinkBridgeEngine.pushClipboard(clipboard)
                    LinkBridgeEngine.setMessage(if (error.isBlank()) "已推送" else "推送失败: $error")
                }
            }) { Text("推送到全部在线设备") }

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

            if (message.isNotBlank()) {
                Text(message, style = MaterialTheme.typography.bodyMedium)
            }
        }
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

private suspend fun resolve(path: String, action: String): String {
    val error = LinkBridgeEngine.resolveConflict(path, action)
    return if (error.isBlank()) "冲突已解决: $path ($action)" else "解决失败: $error"
}
