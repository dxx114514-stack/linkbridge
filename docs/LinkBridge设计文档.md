# LinkBridge（连桥）跨端互联软件 · 系统设计文档

版本：v1.0（设计基线）
目标平台：Linux / Windows / Android
文档对应需求：交付内容 1–10

---

## 1. 整体系统架构设计

### 1.1 架构总览

LinkBridge 采用「单后台服务 + 前端 UI」的进程模型，每台设备既是客户端也是服务端（对等架构，无中心服务器，纯局域网/点对点）。

```
┌─────────────────────────────────────────────────────────────┐
│ UI 层（平台原生）                                            │
│  Linux: Qt6/QML   Windows: Qt6/WinUI   Android: Compose     │
│  —— 仅负责展示与用户交互，通过本地 IPC 调用核心服务            │
├─────────────────────────────────────────────────────────────┤
│ 核心服务层 liblinkbridge-core（C++20，跨平台共享代码）        │
│  ┌──────────┬──────────┬──────────┬──────────┬───────────┐  │
│  │ 配对服务  │ 心跳服务  │ 文件传输  │ 剪贴板同步│ 屏幕控制  │  │
│  ├──────────┼──────────┼──────────┼──────────┼───────────┤  │
│  │ 远程文件管理          │ 文件夹同步引擎      │ 权限/授权  │  │
│  ├──────────┴──────────┼──────────┴──────────┴───────────┤  │
│  │ 设备身份/信任库        │ 配置管理（纯文件，不用环境变量） │  │
│  └──────────┴──────────┴──────────┴──────────┴───────────┘  │
│  ※ 所有业务核心为纯函数 / 独立状态机类，不 include 任何       │
│    Socket 头文件；通过 ITransport 抽象注入传输实现。          │
├─────────────────────────────────────────────────────────────┤
│ 传输与安全层（可被替换、可单测 Mock）                          │
│  配对通道(TLS1.3+PAKE) │ 心跳通道(TLS1.3双向认证) │ SSH通道  │
├─────────────────────────────────────────────────────────────┤
│ 平台适配层 PAL（每平台单独实现，接口统一）                     │
│  文件监听 inotify / ReadDirectoryChangesW / FileObserver     │
│  剪贴板 X11/Wayland/Win32/Android ClipboardManager           │
│  屏幕捕获 X11/Wayland-PipeWire/DxgiDuplication/MediaProjection│
│  密钥托管 libsecret / DPAPI / Android Keystore               │
│  后台与省电 systemd / Win Service / ForegroundService+Doze   │
└─────────────────────────────────────────────────────────────┘
```

### 1.2 关键架构决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 核心语言 | C++20，三端共享同一核心库 | 性能、单一代码库、与 Qt/NDK 天然集成 |
| 对等拓扑 | 无中心服务器，LAN 直连 + 手动 IP | 隐私、零依赖、部署简单 |
| 通道分工 | 配对/心跳用 TLS；业务全部走 SSH | 职责隔离：心跳独立，不阻塞业务通道（需求七） |
| 核心/网络解耦 | 核心状态机 + `ITransport` 依赖注入 | 可单元测试、可替换传输实现 |
| 配置管理 | 全部来自 TOML 配置文件，禁用环境变量 | 可审计、可备份、行为可复现 |
| 元数据存储 | SQLite（WAL 模式） | 事务、跨平台成熟、支持加密（SQLCipher） |
| 敏感数据落盘 | SQLCipher/平台密钥托管加密 | 满足"敏感信息不得明文存储" |

### 1.3 通道与端口规划（默认值，配置文件可改）

| 通道 | 默认端口 | 协议 | 用途 |
|---|---|---|---|
| 配对通道 | 45710/TCP | TLS 1.3 + SPAKE2+（配对码作口令） | 临时配对交换，配对成功即关闭监听或仅响应已配对设备 |
| 心跳通道 | 45711/TCP | TLS 1.3 双向证书认证 | 在线状态检测，轻量 ping/pong |
| 业务通道 | 45712/TCP | SSH2（仅公钥认证） | SFTP 文件传输/远程文件管理、自定义子系统（剪贴板、同步）、VNC 端口转发 |

进程模型：桌面端一个常驻守护进程 `linkbridged` + UI 前端；Android 端一个 ForegroundService（含核心库 JNI）+ Compose UI。

---

## 2. 模块划分与职责说明

| 模块 | 职责 | 关键约束 |
|---|---|---|
| `core/identity` | 首次启动生成 Ed25519 设备密钥对与 SSH 主机密钥；设备指纹（公钥哈希）计算 | 私钥不出安全存储 |
| `core/config` | 加载/校验/热更新 TOML 配置；提供强类型配置对象 | 唯一配置来源，禁止环境变量 |
| `core/truststore` | 已配对设备记录（设备ID、名称、平台、公钥、指纹、授权范围）的增删改查 | SQLCipher 加密落盘 |
| `core/pairing` | 配对状态机：生成配对码、发起/响应配对、校验、写入信任库 | 纯状态机，Socket 由外部注入 |
| `core/heartbeat` | 心跳调度器：间隔配置、自适应调整、失败计数、在线状态发布 | 独立线程，不触碰业务通道 |
| `core/transfer` | 文件互传：任务队列、分块、断点续传、校验、进度回调 | 状态机 + 回调，不直接做 I/O 复用 |
| `core/clipboard` | 剪贴板变化检测策略、去重、体积限制、覆盖确认策略 | 平台剪贴板读写经 PAL 注入 |
| `core/remotefs` | 远程文件管理会话：目录列举/读写/删除/重命名/建目录，路径越权校验 | 全部操作经权限模块裁决 |
| `core/screen` | 屏幕操作/共享会话管理：随机 VNC 密码、SSH 隧道建立、只读/可控制模式、一键断开 | 密码仅存内存，随会话销毁 |
| `core/sync` | 文件夹同步引擎（详见第 9 节）：监听、扫描、增量、冲突、版本、回收站 | 独立状态机，每同步文件夹一个实例 |
| `core/acl` | 权限模型与裁决：能力令牌、目录白名单、功能开关、会话授权 | 所有敏感操作唯一入口 |
| `net/tls` | TLS 1.3 通道封装（配对 PSK/PAKE 模式、设备证书模式） | 实现 `ITransport` |
| `net/ssh` | SSH 客户端/服务端封装（libssh），SFTP、自定义子系统、端口转发 | 实现 `ITransport`/`ISshChannel` |
| `pal/*` | 平台适配层接口与实现（见 1.1 图） | 接口统一，编译期选择实现 |
| `ui/*` | 三端 UI：设备列表、配对向导、传输进度、同步状态、授权弹窗 | 不含任何网络/加密代码 |

---

## 3. 协议设计

### 3.1 设备身份模型

- 首次启动生成：Ed25519 身份密钥对（长期）+ RSA-3072/Ed25519 SSH 主机密钥（供业务通道）。
- `device_id = Base32(SHA-256(身份公钥))[0:16]`，全局唯一。
- 设备指纹 = `SHA-256(device_id ‖ 身份公钥 ‖ ssh_host_pubkey)`，配对时双方交叉校验并展示短指纹（防 MITM）。
- 私钥存储：Windows DPAPI / Linux libsecret / Android Keystore；数据库用 SQLCipher。

### 3.2 配对协议（Pairing Protocol）

设计要点：6 位数字码熵仅 ~20bit，不能直接当密钥使用，必须配合 PAKE 防离线爆破。

```
发起端 A                                     目标端 B
   │  用户在 B 端界面查看/生成配对码 C（6位数字，5分钟有效，单次）
   │  用户在 A 端输入 B 的 IP + 配对码 C
   │
   │── TCP 连接 B:45710 ──────────────────────▶│
   │── PAKE 握手（SPAKE2+，口令=C，A 为 client）▶│  抗离线字典攻击
   │◀──────────── 派生会话密钥 K_enc ──────────│
   │
   │── E(K_enc, PairingRequest{              ──▶│ B 解密并缓存
   │      device_id, name, platform, ip, mac,
   │      ed25519_pubkey, ssh_host_pubkey,
   │      nonce_A, timestamp })               │
   │                                           │ B 端弹窗：显示 A 名称+短指纹，
   │                                           │ 用户确认"接受配对"
   │◀── E(K_enc, PairingResponse{             ─│ 携带"返回请求"标记 return_request=true
   │      device_id_B, name_B, platform_B,     │
   │      ip_B, mac_B, pubkeys_B, nonce_A,     │
   │      nonce_B, return_request=true })      │
   │── E(K_enc, PairingAck{nonce_B, fingerprint_A})▶ 双方互验指纹
   │
   │  双方校验通过 → 互写信任库，配对码立即作废
```

协议规则：

1. **加密**：应用层消息在 PAKE 会话密钥上用 AES-256-GCM 加密封装（即使底层再叠 TLS 也无妨，纵深防御）。
2. **有效期**：配对码 5 分钟过期；`timestamp` 偏移超过 ±120s 拒绝。
3. **单次使用**：配对成功/失败后该码立即作废；同一 IP 1 分钟内最多 5 次尝试，指数退避。
4. **防重放**：双向 nonce 回显 + 时间戳；信任库记录 `paired_at`，旧消息无法重放建立信任。
5. **防 MITM**：双方 UI 展示 `SHA-256(双方公钥拼接)` 的短指纹（6 组数字），人工比对（类 Signal 安全码）。
6. 配对码仅用于 PAKE 口令，**从不作为长期凭证**，配对完成后不再参与任何鉴权。

### 3.3 心跳协议（Heartbeat Protocol）

独立 TCP 连接，TLS 1.3 双向认证（客户端证书 = 配对时登记的身份公钥自签证书，服务端按信任库白名单校验指纹）。

```
消息格式（CBOR 编码，单条 ≤ 128B）：
  Ping  { type:0x01, seq:u32, timestamp:i64, protocol_ver:u8 }
  Pong  { type:0x02, seq:u32, timestamp:i64, capabilities:bitmap }
```

规则：

1. 只对**信任库中的设备**响应；未授权连接在 TLS 握手阶段即拒绝（无法探测在线状态）。
2. 心跳不携带任何业务/敏感数据，`capabilities` 仅含功能开关位（便于 UI 置灰不可用的入口）。
3. 调度策略（配置文件 `heartbeat.toml`）：
   - `interval = 10s / 30s / 60s` 可配；默认 30s。
   - 自适应：连续稳定在线 → 间隔 ×2 直至上限；出现失败 → 回落至基础间隔。
   - 失败判定：连续 `max_failures`（默认 3）次超时（单次超时 5s）→ 标记离线，发布 `DeviceOffline` 事件。
   - 恢复：离线状态下以低频率（每 60s）继续探测；一旦 Pong 成功 → 发布 `DeviceOnline`，UI 刷新，触发同步引擎重连。
4. 移动端省电：Android 使用 `setExactAndAllowWhileIdle` 受限，采用 WorkManager 周期任务（≥15min 后台档）+ 前台服务时 30s 档；网络切换（Wi-Fi↔蜂窝）时重置调度并立即探测一次；蜂窝网络下默认仅 60s 档且可在配置中关闭心跳。

### 3.4 数据传输协议（业务通道）

所有业务复用一条 SSH 连接（libssh，仅 `publickey` 认证，禁用口令/键盘交互认证；known_hosts 即信任库指纹，杜绝 TOFU 提示）。

| 业务 | SSH 机制 |
|---|---|
| 文件互传 | SFTP 子系统 + 自研分块续传层（见下） |
| 远程文件管理 | SFTP 子系统（opendir/read/write/remove/rename/mkdir/stat） |
| 剪贴板同步 | 自定义子系统 `linkbridge-clipboard`（CBOR 消息流） |
| 文件夹同步 | 自定义子系统 `linkbridge-sync`（元数据流 + 块数据传输） |
| 屏幕操作/共享 | SSH 本地端口转发：控制端 `127.0.0.1:随机端口` → 被控端 `127.0.0.1:5900`（VNC over SSH，VNC 仅监听回环） |

文件传输层协议（SFTP 之上的轻量任务协议）：

```
TransferOffer  { task_id, file_name, size, mtime, sha256(整文件,可选), chunk_size }
TransferAccept { task_id, resume_offset }        // 接收端确认 + 断点续传偏移
TransferChunk  { task_id, offset, len, data }    // 默认 chunk 1MB，逐块 CRC32C
ChunkAck       { task_id, offset, crc32c }
TransferDone   { task_id, sha256(整文件) }       // 最终整文件校验
TransferCancel { task_id, reason }
Progress       { task_id, bytes_done, speed }    // 周期性进度事件
```

- 断点续传：接收端落盘临时文件 `<name>.lbpart` + sidecar（已收偏移位图），Offer 时比对已有长度，从 `resume_offset` 续传。
- 取消：任一端发 `TransferCancel`，清理或保留 `.lbpart`（配置决定，默认保留 24h 供续传）。

### 3.5 权限模型（Authorization Model）

**能力（Capability）+ 白名单 + 会话授权**三层：

```
TrustRecord {
  device_id, name, platform, fingerprints, paired_at,
  permissions: {
    file_transfer:   bool,              // 文件互传
    clipboard:       { enabled, mode: auto|confirm, max_bytes },
    remote_fs:       [ { local_path, access: ro|rw } ],   // 目录白名单
    screen_control:  bool,              // 允许被控
    screen_share:    bool,              // 允许被看
    sync_folders:    [ SyncFolderGrant ]// 见第 9 节
  }
}
```

裁决规则：

1. 每个传入 SSH 子系统/SFTP 请求先经 `acl::authorize(device_id, op, path)`。
2. 远程文件管理：规范化路径后强制校验 `realpath` 前缀命中白名单，且禁止符号链接逃逸（`O_NOFOLLOW` / 逐段校验）。
3. 屏幕操作/共享：**每次会话需本端弹窗确认**（或预先授权且未勾选"每次确认"），会话期间被控端常驻可见指示（托盘/悬浮条），提供一键断开与权限即时回收。
4. 权限变更即时生效：ACL 更新事件广播到所有活跃会话，被回收权限的会话立即终止。
5. 敏感操作（删除、覆盖剪贴板、屏幕控制）记入本地审计日志（SQLite，含时间、设备、操作、路径、结果）。

---

## 4. 三端技术选型建议

| 层 | Linux | Windows | Android |
|---|---|---|---|
| 核心库 | C++20（GCC/Clang），CMake | C++20（MSVC/Clang-cl），CMake | 同一核心库，NDK r26+，JNI 封装 |
| UI | Qt6 (QML) 或 GTK4（推荐 Qt6，与 Windows 复用 QML 代码） | Qt6 (QML) 或 WinUI 3（推荐 Qt6 复用；若追求原生观感选 WinUI 3 但 UI 层需重写） | Kotlin + Jetpack Compose |
| TLS | OpenSSL 3 / BoringSSL | OpenSSL 3 / Schannel（推荐 OpenSSL，三端行为一致） | BoringSSL（随 NDK） |
| PAKE | libspake2 / 自研 SPAKE2+（基于 OpenSSL EC） | 同左 | 同左（核心库内实现） |
| SSH | libssh 0.11（服务端支持好） | libssh | libssh（NDK 编译） |
| VNC | LibVNCServer（被控）/ LibVNCClient（主控） | 同左 | 被控端：MediaProjection + 自研 RFB 服务端（复用 LibVNCServer 协议层）；主控：LibVNCClient |
| 屏幕捕获 | X11 XShm / Wayland PipeWire | DXGI Desktop Duplication（回退 GDI） | MediaProjection API |
| 文件监听 | inotify（+fanotify 可选） | ReadDirectoryChangesW | FileObserver（API 29+ 作用域存储内） |
| 剪贴板 | X11 XFixes / Wayland wlr-data-control | Win32 Clipboard API + WM_CLIPBOARDUPDATE | ClipboardManager + OnPrimaryClipChangedListener（注意 Android 10+ 后台读取限制：仅前台服务/输入法等场景可读，需在 UI 明示） |
| 数据库 | SQLite + SQLCipher | 同左 | Room/SQLDelight（UI 层）+ 核心层直接 SQLCipher |
| 密钥托管 | libsecret (gnome-keyring/KWallet) | DPAPI (CryptProtectData) | Android Keystore |
| 后台常驻 | systemd user service | Windows Service / 计划任务 + 托盘 | ForegroundService（`dataSync` 类型）+ WorkManager |
| 配置 | `~/.config/linkbridge/*.toml` | `%APPDATA%\LinkBridge\*.toml` | App 私有目录 `files/config/*.toml` |
| 构建/CI | CMake + vcpkg/conan，GitHub Actions 三平台矩阵 | 同左 | Gradle + CMake externalNativeBuild |

跨端 UI 框架评估结论：**Qt6/QML 可覆盖 Linux + Windows 两端桌面 UI 并复用核心 C++；Android 因后台模型与权限差异大，单独用 Compose**，UI 层薄、共享代码全在核心库，交互稿三端统一。Flutter 方案不采纳：与 C++ 核心和 libssh/VNC 的 FFI 集成成本高，且桌面端系统托盘/权限支持弱于 Qt。

---

## 5. 核心流程时序（文字化）

### 5.1 配对

```
用户A → A-UI：输入 B 的 IP
A-UI → A-UI：展示配对码 C（6位，5min 倒计时，单次）
用户A 将 C 告知/输入到 B（或反向：B 生成，A 输入——双向等价，以下按 A 生成、B 发起亦可，协议对称）
B-UI → pairing::initiate(ip_A, C)
B-core → A:45710：TCP → SPAKE2+ 握手（口令=C）→ K_enc
B-core → A-core：E(K_enc, PairingRequest{身份包, nonce_B})
A-core：解密、缓存、校验时间戳 → A-UI 弹窗"设备 B 请求配对，指纹 123-456，接受？"
用户A 确认 → A-core → B-core：E(K_enc, PairingResponse{身份包, nonce_B, return_request=true})
B-core：回显校验 nonce_B → B → A：PairingAck{nonce_A? 回显, fingerprint}
双方：交叉校验指纹 → 写入信任库 → 作废配对码 → UI 提示"已添加受信任设备"
任一步失败：作废配对码，记录审计日志，UI 提示原因
```

### 5.2 心跳与在线状态

```
heartbeat 调度器（每设备一个定时任务，间隔=配置值×自适应系数）
  → 对信任库每台设备：TLS 1.3 双向认证连接 45711
  → 握手失败/超时(5s)：fail_count++；≥3 次 → 置离线，发 DeviceOffline 事件 → UI 灰显
  → 握手成功：发送 Ping{seq,ts} ← Pong{seq,ts,caps}
  → 校验 seq 回显 → RTT 记录 → fail_count=0
  → 若此前离线：发 DeviceOnline 事件 → UI 点亮 → 通知 sync 引擎对该设备重连、clipboard 恢复推送
  → 自适应：连续 10 次成功且 RTT 稳定 → interval×2（上限 5min）；任何失败 → 回落基础值
移动端：WorkManager 唤起时批量探测一轮，随后休眠；前台服务活跃期走 30s 档
```

### 5.3 文件互传

```
A 用户选文件 → A-core transfer::send(B, paths)
A → B（SSH/自定义子系统）：TransferOffer{元数据}
B-core：acl 校验 file_transfer 权限 → B-UI 弹窗"接收 3 个文件（共 1.2GB）？"
B 确认 → TransferAccept{resume_offset（若存在 .lbpart）}
A → B：TransferChunk 流（流水线窗口=8 块）↔ ChunkAck
周期性：Progress 事件 → 双端 UI 进度条/速度
全部完成：TransferDone{sha256} → B 整文件校验 → 重命名 .lbpart → 完成通知
异常：连接断开 → 保留 .lbpart 与位图，重连后从断点续传；Cancel → 按策略清理
```

### 5.4 剪贴板同步

```
PAL 监听本地剪贴板变化（事件驱动 + 2s 轮询兜底）
  → clipboard 核心：去抖（500ms 内连续变化只取最后）、去重（与上次哈希相同跳过）、
    体积检查（> max_bytes，默认 16MB → 丢弃并提示）
  → 遍历信任库中 clipboard.enabled 且在线的设备：并行各建/复用 SSH 连接
  → 发送 ClipboardPush{mime_type, payload, content_hash, ts}
接收端核心：acl 校验 → 若 mode=auto：直接写本地剪贴板，托盘提示"已同步来自 A 的剪贴板"
          若 mode=confirm：弹窗预览，用户确认后覆盖
回环防护：写入远端剪贴板触发的本地变化事件标记 origin=remote，TTL 内不再回推
图片：统一转 PNG 传输；Android 端受后台限制时仅在前台服务活跃期同步
```

### 5.5 屏幕操作 / 屏幕共享

```
控制端 A → 被控端 B（SSH 控制消息）：ScreenSessionRequest{mode: control|view}
B-core：acl 校验 screen_control/screen_share → B-UI 弹窗"A 请求查看/控制你的屏幕"
B 确认 → B-core：生成随机 16 字符 VNC 密码（仅内存）→ 拉起 LibVNCServer（仅监听 127.0.0.1:5900）
B → A：ScreenSessionAccept{vnc_password}（经 SSH 加密通道传递）
A-core：SSH 本地端口转发 127.0.0.1:随机端口 → B:5900 → LibVNCClient 连接 → UI 渲染
mode=view：VNC 服务端以只读方式运行（忽略一切输入事件）；mode=control：注入键鼠
会话期间：B 端常驻悬浮条"屏幕正被 A 查看/控制 [断开]"
断开：任一端发起 / 权限被回收 / SSH 断开 → 关闭转发、销毁 VNC 实例、清零密码、审计日志
"锁定"（B 主动暂停输入但保持画面）：B 发送 SessionPauseInput，A 端输入事件被服务端丢弃
```

### 5.6 文件夹同步

```
建立：A 创建同步文件夹请求{folder_id, 本地路径, 模式, 忽略规则} → 经 SSH 发至 B
      B 用户确认并选择本地映射路径、方向权限 → 双方写入 SyncFolderGrant → 引擎启动
运行（每个文件夹一个状态机实例，详见第 9 节）：
  1. PAL 文件事件（inotify/RDCW/FileObserver）→ 防抖队列 → 增量扫描受影响路径
  2. 定时全量扫描（默认 1h，可配）兜底：遍历目录 → 与 SQLite 索引比对(size/mtime) → 疑似变更计算哈希
  3. 生成变更集 → 与对端交换 Index 摘要（Merkle/哈希清单）→ 计算差集
  4. 按差集拉取/推送块数据（复用传输层：分块、续传、校验、限速）
  5. 冲突检测：同一文件两端版本向量并发 → 按策略处理 → 冲突列表事件 → UI
  6. 覆盖/删除的旧版本 → 移入 .lbfolder/.versions 或回收站，按保留策略清理
  7. 状态事件（扫描中/同步中/完成/冲突/错误）实时推 UI
手动同步：UI 触发 → 立即执行一次全量扫描+同步
设备离线：队列挂起；DeviceOnline 事件 → 自动续传
```

---

## 6. 关键接口定义

### 6.1 核心抽象（C++，核心层不依赖任何 Socket 头文件）

```cpp
// 传输抽象：由 net/tls 或 net/ssh 实现，核心层仅面向此接口编程
struct ITransport {
  virtual ~ITransport() = default;
  virtual Expected<ByteBuf> request(std::string_view subsystem,
                                    ByteBuf payload, Millis timeout) = 0;
  virtual Expected<void>    send_stream(std::string_view subsystem,
                                        IChunkSource&, IProgressCb&) = 0;
};

// 设备在线状态
enum class Presence { Online, Offline, Unknown };
struct IHeartbeatService {
  virtual void set_interval(DeviceId, Seconds base) = 0;
  virtual Presence presence(DeviceId) const = 0;
  virtual Subscription on_presence_changed(PresenceCb) = 0;  // 事件订阅
};

// 配对状态机
enum class PairState { Idle, CodeShown, Handshaking, AwaitConfirm,
                       Exchanging, Verifying, Done, Failed };
struct IPairingService {
  virtual PairCode  generate_code(Ttl = 5min) = 0;                    // 本端为响应方
  virtual Result    initiate(IpAddr, PairCode) = 0;                   // 本端为发起方
  virtual void      confirm(bool accept) = 0;                         // UI 确认回调
  virtual PairState state() const = 0;
  virtual Subscription on_state(StateCb) = 0;
};

// 文件传输
struct ITransferService {
  virtual TaskId offer(DeviceId, std::vector<FilePath>) = 0;
  virtual void   accept(TaskId) / decline(TaskId) / cancel(TaskId) = 0;
  virtual Subscription on_progress(ProgressCb) = 0;
};

// 文件夹同步
struct ISyncService {
  virtual Result add_folder(SyncFolderConfig) = 0;
  virtual void   remove_folder(FolderId) = 0;
  virtual void   set_mode(FolderId, SyncMode) = 0;
  virtual void   scan_now(FolderId) = 0;                 // 手动同步
  virtual SyncFolderStatus status(FolderId) const = 0;   // 状态+统计
  virtual std::vector<ConflictItem> conflicts(FolderId) = 0;
  virtual void   resolve_conflict(ConflictId, Resolution) = 0;
  virtual Subscription on_folder_event(FolderEventCb) = 0;
};

// 权限裁决（所有敏感操作唯一入口）
struct IAcl {
  virtual bool authorize(DeviceId, Operation, ResourcePath) = 0;
  virtual void grant(DeviceId, PermissionSet) / revoke(DeviceId, PermissionSet) = 0;
  virtual Subscription on_revoked(RevokeCb) = 0;         // 供会话管理立即断开
};
```

### 6.2 控制消息 Wire Format（CBOR，经加密通道）

```jsonc
// 通用信封
{ "v": 1, "type": "<msg_type>", "msg_id": "uuid", "ts": 1735000000, "body": { } }

// 剪贴板
{ "type": "clipboard.push",
  "body": { "mime": "text/plain|image/png", "size": 1234,
            "sha256": "...", "payload": "<base64>" } }

// 同步索引摘要
{ "type": "sync.index.summary",
  "body": { "folder_id": "uuid", "seq": 42,
            "entries": [ { "path": "a/b.txt", "size": 100, "mtime": 1735000000,
                           "hash": "blake3:...", "version": [3, 5] } ] } }

// 同步块请求
{ "type": "sync.block.request",
  "body": { "folder_id": "uuid", "path": "a/b.txt",
            "blocks": [ { "offset": 0, "len": 1048576, "hash": "blake3:..." } ] } }
```

### 6.3 配置文件（示例 `linkbridge.toml`）

```toml
[network]
pair_port = 45710
heartbeat_port = 45711
ssh_port = 45712
bind_addr = "0.0.0.0"

[heartbeat]
interval_sec = 30          # 10/30/60 可选
max_failures = 3
adaptive = true
mobile_metered_interval_sec = 60

[transfer]
chunk_size = 1048576
pipeline_window = 8
max_concurrent_tasks = 3
rate_limit_kbps = 0        # 0 = 不限速
resume_part_ttl_hours = 24

[clipboard]
enabled = true
mode = "confirm"           # auto | confirm
max_bytes = 16777216

[screen]
require_confirm_each_session = true
vnc_only_loopback = true

[sync]
rescan_interval_sec = 3600
max_file_size_mb = 4096
max_concurrent_files = 4
max_concurrent_devices = 2
rate_limit_kbps = 0
default_conflict_policy = "keep_both"   # keep_both|newer|larger
version_keep_count = 5
version_keep_days = 30
trash_enabled = true
trash_max_mb = 2048
```

---

## 7. 安全风险清单及缓解方案

| # | 风险 | 威胁场景 | 缓解方案 |
|---|---|---|---|
| 1 | 配对码暴力破解 | 6 位码仅 10^6 空间，在线/离线字典攻击 | SPAKE2+ PAKE（口令不直接派生密钥，离线不可验证）；单 IP 限速 5 次/min 指数退避；5min 过期；单次使用；失败即作废 |
| 2 | 配对 MITM | 攻击者中转配对流量 | PAKE 本身抗被动窃听；双端 UI 展示公钥短指纹人工比对；信任库绑定指纹，后续连接校验 |
| 3 | 重放攻击 | 重放配对/心跳/同步消息 | 双向 nonce 回显 + 时间戳窗口(±120s)；消息单调序号；配对码一次性 |
| 4 | 未授权探测/扫描 | 伪造心跳探活、端口扫描指纹识别 | 心跳 TLS 双向证书认证，非信任设备握手即拒、不返回任何信息；服务端口默认仅响应信任库指纹 |
| 5 | SSH 主机密钥替换 | 重配设备或 ARP 欺骗后冒充 | 严格 known_hosts（信任库指纹）比对，不匹配即断开并红色告警，禁止 TOFU 自动接受 |
| 6 | 路径越权/目录穿越 | 远程文件管理访问 `../../etc` | realpath 规范化 + 白名单前缀校验 + 禁符号链接逃逸（O_NOFOLLOW 逐段解析）；禁止系统关键目录硬编码黑名单（/, /etc, /proc, C:\Windows 等） |
| 7 | 剪贴板泄露 | 密码管理器复制的口令被同步到不可信端 | 敏感标记跳过（Windows CF_EXCLUDECLIPBOARD / Android `EXTRA_IS_SENSITIVE`）；按目标设备粒度开关；confirm 模式预览；max_bytes 限制 |
| 8 | 屏幕控制滥用 | 已配对设备被恶意发起控制 | 每次会话弹窗确认（默认）；被控端常驻可见指示 + 一键断开；只读/控制分离授权；会话审计 |
| 9 | 同步泛洪/资源耗尽 | 海量小文件或超大文件拖垮设备 | 限速、并发上限、单文件体积上限、磁盘空间预检（不足即错误态）、忽略规则默认含临时文件 |
| 10 | 同步越权写 | 对端推送未授权路径 | 每文件夹独立授权；写入前校验路径属于该 folder 映射根；接收端模式禁止本端修改同步目录（PAL 层只读保护尽力而为 + 扫描告警） |
| 11 | 敏感数据落盘明文 | 信任库/审计日志/配置被窃取 | SQLCipher 加密数据库；私钥走平台安全存储（DPAPI/libsecret/Keystore）；IP/MAC 入库前哈希脱敏（展示层才用原文，且仅存内存） |
| 12 | VNC 密码泄露 | 内存转储/日志泄露 | 密码仅存内存、会话结束清零（explicit_bzero）；永不写日志；VNC 仅监听回环、必须经 SSH 隧道 |
| 13 | 移动端后台与电量 | Doze 模式心跳失效/耗电超标 | WorkManager 调度、前台服务声明 dataSync 类型、蜂窝下降频、网络切换重调度、用户可配省电模式 |
| 14 | 旧版本协议降级 | 强制使用弱加密 | 协议版本协商仅允许 ≥v1 且 TLS1.3；算法套件白名单（AES-256-GCM/ChaCha20-Poly1305，X25519/Ed25519） |
| 15 | 删除/覆盖不可恢复 | 同步误删重要文件 | 回收站 + 版本控制（数量/天数/容量三重上限）；冲突默认保留双方 |

---

## 8. 开发路线图与里程碑

> 原则：每个里程碑端到端可演示；核心库先行，UI 跟随；测试与功能同里程碑交付。

| 里程碑 | 内容 | 验收标准 |
|---|---|---|
| **M1 地基** | 构建系统（CMake 三端 + Android NDK）；配置模块；身份/信任库（SQLCipher）；`ITransport` 抽象 + TLS 实现；日志 | 桌面两端互发加密 echo；密钥安全落盘；单元测试覆盖核心模块 ≥70% |
| **M2 配对+心跳** | SPAKE2+ 配对状态机；配对 UI 向导；心跳服务（含自适应）；设备列表 UI 在线状态 | 桌面两端完整配对、指纹比对、在线/离线实时切换；弱网模拟测试通过 |
| **M3 文件互传 + 剪贴板** | SSH 通道（libssh）；SFTP 分块续传；传输任务 UI；剪贴板监听/同步/确认策略 | 1GB 文件断网续传成功；双端剪贴板文本/图片互通 |
| **M4 远程文件管理** | SFTP 文件操作 API；ACL 目录白名单；远程文件浏览器 UI | 越权访问用例全部被拒；增删改查重命名全通过 |
| **M5 屏幕共享/操作** | VNC 服务端/客户端集成；SSH 隧道；随机密码交换；会话确认/指示/断开 UI | 桌面两端互控延迟可接受；只读模式输入注入全部无效 |
| **M6 文件夹同步 MVP** | 同步引擎状态机；监听+定时扫描；索引交换；双向同步；基础冲突（keep_both）；SQLite 索引 | 双端 10k 文件初始同步 + 增量同步正确；冲突双方保留 |
| **M7 同步完整版** | 四种模式；版本控制/回收站；忽略规则；限速/并发控制；冲突 UI 与手动解决 | 第 9 节全部需求用例通过；长稳 72h 无内存泄漏 |
| **M8 Android 端** | NDK 核心集成；Compose UI；前台服务 + WorkManager；MediaProjection 共享；FileObserver | Android↔桌面配对/传输/剪贴板/同步全通；Doze 下心跳策略生效 |
| **M9 安全加固与发布** | 第 7 节风险清单逐项回归；模糊测试（协议解析）；性能调优；安装包/签名；用户文档 | 渗透测试清单通过；三端安装包就绪 |

---

## 9. 文件夹同步模块设计

### 9.1 同步模式（SyncMode）

| 模式 | 本端可写 | 对端可写 | 行为 |
|---|---|---|---|
| `send_receive`（双向） | ✔ | ✔ | 任意修改互相同步；并发修改进入冲突处理 |
| `send_only`（发送端） | ✔ | 修改被覆盖 | 本端为权威源；对端本地修改在下次同步时被本端版本覆盖（先经对端确认或按预设策略），本端不拉取对端变更 |
| `receive_only`（接收端） | ✘（尽力防护） | ✔ | 仅接收对端内容；检测到本端修改 → 告警 UI，不对外传播，可选"还原为对端版本" |
| `mirror`（单向镜像） | 源端 ✔ | ✘ | 目标端与源端严格一致：源端删除 → 目标端删除（进回收站/版本库）；用于备份 |

### 9.2 变更检测

- **事件驱动（主）**：PAL 统一接口 `IFileWatcher::watch(root) → events{Created,Modified,Deleted,Renamed}`，Linux inotify / Windows ReadDirectoryChangesW / Android FileObserver。事件入防抖队列（500ms 聚合），Renamed 尽量配对为 rename 而非 delete+create（保版本链）。
- **定时扫描（兜底）**：`rescan_interval_sec`（默认 3600s）全量遍历；与 SQLite 索引比对 `(size, mtime)`，不一致才计算内容哈希（避免全量哈希开销）。
- **哈希策略**：文件 ≤ 4MB 整体 BLAKE3；> 4MB 分块（默认 1MB 固定块，后续可升级 CDC 滚动哈希）逐块 BLAKE3，文件哈希 = 块哈希列表的 BLAKE3。
- **元数据**：`path, size, mtime, mode/perm, hash, version_vector, flags`；`sync.xattrs` 可选。未授权目录的产物绝不进入索引。

### 9.3 索引与差量

- 每文件夹一个索引表（SQLite）。设备间交换 `IndexSummary`（路径+哈希+版本向量），双集合 diff 得出 `need_pull / need_push / conflicts`。
- 传输只发差异块：对端已有文件的块哈希可作为弱种子复用（同目录重命名/复制零传输）；断点续传沿用第 3.4 节块协议。

### 9.4 冲突处理

- **判定**：版本向量并发（双方互不包含）→ 冲突。
- **默认策略**（`default_conflict_policy`，可每文件夹覆盖）：
  - `keep_both`（默认）：保留双方，后写入者重命名为 `name (conflict <device_name> <yyyyMMdd-HHmmss>).ext`；
  - `newer`：保留 mtime 较新者，旧版进版本库；
  - `larger`：保留较大者（仅文档类场景，默认不启用）。
- **冲突列表**：引擎发 `FolderConflicted` 事件 → UI 列表（路径、两端设备、mtime、大小、预览入口）→ 用户可选 `保留本端 / 保留对端 / 保留双方 / 手动合并（外部工具）/ 删除`。
- 删除 vs 修改：删除方与修改方并发 → 默认保留修改版，删除意图进版本库（可恢复）。

### 9.5 版本控制与回收站

- 被覆盖/删除的文件移入 `<sync_root>/.linkbridge/versions/`（按 `原路径/时间戳` 组织），元数据入库。
- 保留策略三重上限，先到先清（LRU）：`version_keep_count`（每文件最近 N 版，默认 5）、`version_keep_days`（默认 30 天）、`trash_max_mb`（版本库总容量，默认 2048MB，超限删最旧）。
- `trash_enabled=true` 时删除操作先移回收站；`mirror` 模式下目标端删除同样进版本库，保证可回滚。
- UI 提供"浏览历史版本 / 还原 / 彻底删除"。

### 9.6 权限与路径映射

```
SyncFolderGrant {
  folder_id: uuid,
  label: "文档",
  mode: send_receive,                 // 本端视角
  local_path: "/home/u/Docs",
  peers: [ { device_id, remote_path: "D:\\Docs",
             access: read_write | read_only | send_only } ],
  ignore: ["*.tmp", ".git/", "~$*", "node_modules/", ".*" ],  // .stignore 风格
  max_file_size_mb, rate_limit_kbps   // 可覆盖全局
}
```

- 建立文件夹必须**双端显式确认**：发起方定义 folder_id/模式/忽略规则 → 对端弹窗选择本地映射路径与 access → 双方落库才启动。
- `read_only`：该对端只可拉取；`send_only`：该对端只可推送来源变更。方向裁决在**接收侧**强制执行（不信任对端自律）。
- 路径校验：所有对端发来的路径经 `normalize + 前缀校验 + 禁 symlink 逃逸`；两端路径可完全不同，仅以 folder_id 关联。
- 忽略规则双端各自生效并交换摘要展示差异，规则不匹配的变更不进索引、不传输。

### 9.7 同步文件夹状态机

```
                 ┌──────────┐  添加/启动    ┌─────────┐
                 │ (未创建) │ ───────────▶ │ Scanning│◀────────────┐
                 └──────────┘              └────┬────┘             │
                                                │ 扫描完成          │ 周期/手动重扫
                                                ▼                  │
                    ┌─────────┐  有待同步项   ┌───────┐            │
                    │  Idle   │ ◀──────────── │ Diff  │ ───────────┘
                    └────┬────┘               └───┬───┘
        DeviceOffline ▲  │ 设备在线且有待同步      │ 生成变更集
                 挂起 │  ▼                        ▼
                    ┌──────────┐  传输完成  ┌─────────┐
                    │ Paused   │            │ Syncing │ ──▶ Idle（发 Synced 事件）
                    └──────────┘            └────┬────┘
                                                 │ 检测到冲突          │ I/O 错误
                                                 ▼                    ▼
                                          ┌───────────┐        ┌─────────┐
                                          │ Conflicted│        │ Errored │
                                          └─────┬─────┘        └────┬────┘
                                                │ 全部解决           │ 重试/用户处理
                                                └────────▶ Idle ◀───┘
```

状态事件携带统计：`{ pending_files, pending_bytes, speed_bps, last_synced_at, error_reason }`。
错误分类：`PermissionDenied / PathNotFound / DiskFull / ConflictLimit / NetworkLost`，UI 显示具体原因与处理入口。

### 9.8 资源控制

- 限速：令牌桶（每文件夹/全局两级）；并发：`max_concurrent_files`、`max_concurrent_devices`；大文件：`max_file_size_mb` 超限跳过并提示。
- 磁盘预检：同步前估算目标所需空间，不足 → Errored(DiskFull)。
- 移动端：充电+Wi-Fi 才执行大文件同步（可配）；Doze 下挂起，前台服务活跃期执行；尊重 Storage Access Framework 授权范围。

---

## 10. 与现有子系统的集成方案

```
                       ┌────────────────────────────────────────┐
                       │           EventBus（核心内发布订阅）     │
                       └────────────────────────────────────────┘
   DeviceOnline ────────────────┬───────────────┬────────────────┤
   DeviceOffline ───────────────┤               │                │
   AclRevoked ──────────────────┤               │                │
                                ▼               ▼                ▼
┌───────────┐ 复用    ┌─────────────────┐  ┌──────────────┐  ┌──────────┐
│ 配对/信任库│───────▶│  Sync 引擎       │  │ Clipboard    │  │ Transfer │
│ 提供设备清单│       │ - 仅对已配对设备  │  │ - 复用 SSH 连接│  │ 复用块协议│
│ 与指纹校验  │       │   开放 sync 子系统│  │   池          │  │          │
└─────┬─────┘        │ - 经 SSH 通道传输 │  └──────────────┘  └──────────┘
      │              │ - ACL 每文件夹裁决│
      ▼              │ - 心跳事件驱动重连│
┌───────────┐        └─────────────────┘
│ ACL 模块  │◀──── 所有子系统唯一裁决入口
└───────────┘
```

集成要点：

1. **与配对/信任库**：同步文件夹授权（`SyncFolderGrant`）挂在 TrustRecord 下；设备被移除 → 级联停用其所有同步授权，保留本地文件与版本库（不删数据）。
2. **与 SSH 通道**：同步复用 SSH 连接池与子系统 `linkbridge-sync`；认证、指纹校验、加密由通道层保证，同步引擎零加密代码。
3. **与设备授权（ACL）**：`sync.index/block` 每条消息首过 `acl::authorize(device, SyncRead/SyncWrite, folder_id)`；`AclRevoked` 事件 → 引擎立即中止该设备会话并暂停队列。
4. **与在线状态检测**：订阅 `DeviceOnline` → 触发该设备所有挂起文件夹立即 diff+续传；`DeviceOffline` → 状态机进 Paused，传输断点信息落盘，恢复后续传。心跳模块与同步引擎无线程/锁共享，仅经 EventBus 通信，杜绝业务阻塞心跳、心跳拖慢业务。
5. **与权限系统**：文件夹创建双端确认流程与配对确认 UI 复用同一确认组件；审计日志统一记录同步的写/删操作。
6. **与 UI**：所有子系统状态经统一 `StatusProvider` 聚合 → 三端 UI 同一套状态模型（设备在线点、传输进度、同步状态徽标），保证交互一致。

---

### 附：需求追溯矩阵

| 需求 | 落点 |
|---|---|
| 配对 6 位码/有效期/单次/防重放 | §3.2、§7-1/3 |
| 心跳可配间隔/自适应/移动端省电/加密防伪 | §3.3、§7-4/13 |
| 文件互传 SSH/进度/断点/校验/取消 | §3.4、§5.3 |
| 剪贴板多类型/并行推送/确认策略/体积限制 | §5.4、§6.3、§7-7 |
| 远程文件管理/授权/目录范围 | §3.5、§7-6 |
| 屏幕操作 VNC over SSH/随机密码/断开回收 | §5.5、§7-8/12 |
| 屏幕共享只读/可见状态/随时停止 | §5.5 |
| 文件夹同步全项（6.1–6.8） | §9、§6.3、§7-9/10/15 |
| 敏感信息不明文存储 | §3.1、§7-11 |
| 配置全部来自文件 | §6.3、§1.2 |
| 核心与网络解耦 | §1.1、§6.1 |

---

## 附录 A：v0.1 实现说明（M1 地基 + M2 配对/心跳 + M3 文件互传 + M4 剪贴板同步 + M5 屏幕共享 + M6 文件夹同步 + M7 同步完整版 + M8 Android 端）

本附录记录已落地代码与上文设计基线的差异，便于评审与后续升级。

> 实施节奏说明：原文里程碑中“文件互传 + 剪贴板”合并为 M3；实际落地拆分为 M3 = 文件互传、M4 = 剪贴板同步、M5 = 只读屏幕共享、M6 = 文件夹同步、M7 = 同步完整版（删除传播 / 版本库 / 冲突策略）、M8 = Android 端（工程骨架 + NDK/JNI 适配，源码级交付），原 M4 远程文件管理与后续屏幕控制顺延。

### A.1 已实现范围

| 能力 | 状态 |
|---|---|
| TOML 子集配置加载 + 校验 + 默认配置生成 | 已实现 |
| 设备身份（ECDSA P-256 长期密钥） | 已实现 |
| 受信任设备库（整体加密落盘） | 已实现 |
| TCP + 4 字节长度前缀分帧 | 已实现 |
| 安全通道（双向认证 + AES-256-GCM） | 已实现 |
| 6 位码配对（有效期 / 尝试上限 / 单次有效 / 指纹比对） | 已实现 |
| 心跳（自适应间隔 / 失败阈值 / 离线探测 / 在线事件） | 已实现 |
| 文件互传（OFFER/ACCEPT/CHUNK/ACK/DONE、断点续传、进度事件） | 已实现 |
| 传输完整性（块级 CRC32C + 整文件 SHA-256）与 `.lbpart` 临时文件 | 已实现 |
| 剪贴板同步（事件驱动 + 2s 兜底轮询、500ms 去抖、哈希去重、回环抑制） | 已实现 |
| 剪贴板文本（UTF-8）与图片（CF_DIB/CF_DIBV5）双向同步 | 已实现 |
| 剪贴板 `auto`/`confirm` 策略、`max_bytes` 上限、敏感内容跳过（Windows） | 已实现 |
| 只读屏幕共享（VIEW/ACCEPT/FRAME_REQUEST/FRAME/STOP，会话令牌绑定、`max_fps` 限速、单帧 `max_bytes` 上限） | 已实现 |
| Windows 屏幕采集（GDI 虚拟桌面 `BitBlt`，输出 32 位 top-down DIB） | 已实现 |
| 查看端落盘最新帧为 BMP（控制台无 GUI，`latest.bmp`） | 已实现 |
| 屏幕共享权限校验（`kPermScreenShare`）+ 每会话确认 + 会话开始/结束事件 | 已实现 |
| `[screen]` 配置（`enabled` / `require_confirm_each_session` / `max_fps` / `max_bytes`）与 `screen_port` 校验 | 已实现 |
| 文件夹同步（HELLO/MANIFEST/PLAN/FILE_START/FILE_CHUNK/FILE_END/APPLY_OK/DONE，双向增量） | 已实现 |
| 同步引擎（`ReadDirectoryChangesW` 监听 + 500ms 轮询 + 1s 防抖 + 定时全量扫描） | 已实现 |
| 同步索引（`LBS2` 自研二进制，落盘 `<data_dir>/sync_index.bin`，记录路径/大小/mtime/SHA-256/冲突标记/墓碑 `deleted`+`deleted_at`） | 已实现 |
| 基础冲突策略 `keep_both`：双方保留 + 冲突副本 `stem (conflict 设备 时间戳)` + 冲突标记防重复 | 已实现 |
| M7 冲突策略 `keep_both` / `newer` / `larger`（`newer` 比较 mtime→size→hash，`larger` 比较 size→mtime→hash，hash 字典序小者胜以保证确定性；仅响应方计算） | 已实现 |
| M7 删除传播：清单一侧以墓碑（`deleted`+`deleted_at`）表达删除；计划新增 `want_delete` / `send_delete` 两组删除列表；删除 vs 修改冲突时修改优先 | 已实现 |
| M7 版本库/回收站（`<sync_root>/.linkbridge/versions/<id>/<原路径>`，覆盖/删除前归档；`version_keep_count` / `version_keep_days` / `trash_max_mb` 三重上限，最旧优先清理） | 已实现 |
| M7 冲突处理 CLI：`sync conflicts` / `sync resolve <路径> <keep_local\|keep_remote\|keep_both>`；版本库 CLI：`sync history [路径]` / `sync restore <路径> [版本ID]` | 已实现 |
| M7 传输限速：`rate_limit_kbps`（KiB/s）按单文件已发送字节数与起始时间补偿 sleep | 已实现 |
| 忽略规则（`.stignore` 风格 `*`/`**`/`?`/尾随 `/`/`!` 取反，配置为逗号分隔）、路径净化、单文件上限 | 已实现 |
| `[sync]` 配置（`enabled` / `folder` / `mode` / `ignore` / `max_file_size_mb` / `rescan_interval_sec` / `default_conflict_policy` / `max_concurrent_files` / `trash_enabled` / `version_keep_count` / `version_keep_days` / `trash_max_mb` / `rate_limit_kbps`）与 `sync_port` 校验 | 已实现 |
| 同步权限校验（`kPermSync`）与 `SyncStarted` / `SyncCompleted` / `SyncFailed` 事件 | 已实现 |
| 控制台 CLI（init / show / devices / code / pair / send / clip / screen / sync / daemon） | 已实现 |
| 单元测试 55 例 + 真实 TCP 端到端 125 项 | 全部通过 |
| M8 Android 工程骨架（Gradle Kotlin DSL + AGP 8.5.2 + Kotlin 1.9.24 + Compose BOM 2024.06.00 + `externalNativeBuild`） | 已实现（Gradle 9.4.1 构建 `assembleDebug` 通过；MuMu 12 模拟器上服务启动与 UI 验证通过） |
| M8 NDK 构建（根 `CMakeLists.txt` 以 `LB_PLATFORM=android` 分支，`android/app/src/main/cpp/CMakeLists.txt` 引入核心库并产出 `lbcore_jni`） | 已实现 |
| M8 POSIX 传输（`src/net/tcp_posix.cpp`，4 字节大端长度帧、`SO_RCVTIMEO/SO_SNDTIMEO`、`MSG_NOSIGNAL`，与 Windows 语义一致） | 已实现 |
| M8 Android 路径注入（`android_set_paths`，配置落 `<filesDir>/config/linkbridge.toml`，首启生成 `sync.enabled=true` 且同步目录为 `<filesDir>/sync`） | 已实现 |
| M8 JNI 桥 `lbcore_jni`（`nativeSetPaths/Start/Stop/IsRunning/IdentityJson/DevicesJson/RequestPairCode/Pair/SendFile/SyncNow/StatusJson/ConflictsJson/ResolveConflict/HistoryJson/RestoreVersion/PushClipboard/RespondConfirm/SetAutoAccept`） | 已实现 |
| M8 事件桥（核心事件统一序列化为 JSON 回调 Kotlin `NativeBridge.onNativeEvent`）与确认桥（`confirm_request` + `nativeRespondConfirm`，60s 超时视为拒绝） | 已实现 |
| M8 Compose UI（启动/停止/刷新、状态、配对码与配对、设备列表、剪贴板推送、屏幕共享入口、冲突三选一解决、事件消息、确认弹窗） | 已实现（编译通过） |
| M8 前台服务（`LinkBridgeService` `dataSync`）+ WorkManager 心跳巡检（15 分钟周期，未运行则尝试拉起） | 已实现 |
| M8 MediaProjection 屏幕共享（`ScreenShareService` `mediaProjection` + ImageReader RGBA→BGRA + 40 字节 `BITMAPINFOHEADER` DIB 输出） | 已实现 |
| M8 FileObserver 同步监听（递归注册目录监听 + 原生 `nativeOnChange(handle)` 回调动量扫描） | 已实现 |
| M8 Android 加密原语（JCA：`MessageDigest`/`Mac`/`Cipher`/`Signature`/`KeyAgreement`，HKDF 仍为纯 C++） | 已实现 |
| M8 Android Keystore（`SecretStore` AES-256-GCM，`iv(12)+ct+tag(16)`）与剪贴板桥（`ClipboardBridge` 文本读写 + `OnPrimaryClipChangedListener` 轮询序号） | 已实现 |

### A.2 关键差异与替代原因

| 设计基线 | v0.1 实现 | 原因与升级路径 |
|---|---|---|
| TLS 1.3（OpenSSL / BoringSSL） | 自研 STS 双向认证握手 + AES-256-GCM 分帧 | 构建环境无 OpenSSL 且无网络下载。握手 = 临时 ECDH + 长期身份签名 + 转录哈希绑定，具备前向保密与身份绑定。`ByteStream` 抽象不变，可整体替换为 TLS 1.3。 |
| SPAKE2+（6 位码零知识） | ECDH + 口令绑定 HKDF（info 含配对码）+ 双向 HMAC 确认 | Windows CNG 不提供任意 EC 点运算，无法实现 SPAKE2。当前防线：码单次有效 + 最多 3 次尝试 + 错误即作废 + 双端指纹人工比对。残余风险：主动 MITM 可在码有效期内离线试码（至多 3 次）。计划以 SPAKE2+ / OPAQUE 升级。 |
| SQLCipher 加密数据库 | 二进制记录序列化，整文件经 DPAPI 加密 | 环境无 SQLite；当前设备量（数十台）无需查询能力。规模增长后迁移至 SQLCipher 或加密 KV。 |
| libssh 文件通道 / SFTP（§3.4） | STS 加密业务通道（独立端口 45713）+ 自研 OFFER/ACCEPT/CHUNK/ACK/DONE | 构建环境无 libssh。断点续传（`.lbpart` 前缀）、块级 CRC32C、整文件 SHA-256、进度/取消语义均按基线保留；`TransferService` 接口不变，后续可换成 SFTP 承载。远程文件管理（§3.5）仍待实现。 |
| 剪贴板经 SSH 通道、图片用 PNG（§5.4） | 独立 STS 加密通道（`clipboard_port`，默认 45714）；图片传原始 DIB | 无 libssh 且无 PNG 编解码依赖。DIB 仅适用于 Windows 双端；`ClipboardContent` 以 mime 标识类型，后续可增加 `image/png` 编解码而不改动同步逻辑。 |
| 剪贴板变化事件（§5.4） | Win32 `WM_CLIPBOARDUPDATE` + `AddClipboardFormatListener`，2s 超时兜底轮询 | 与设计一致；事件唤醒 + 兜底轮询兼顾实时性与丢失通知的容错。 |
| VNC 屏幕通道（§5.5） | 独立 STS 加密通道（`screen_port`，默认 45715）+ 自研 VIEW/FRAME_REQUEST/FRAME 协议；查看端把最新帧写为 BMP | 构建环境无 libssh / LibVNCServer / LibVNCClient。只读由协议天然保证（无任何输入注入指令）；`ScreenService` 与 `IScreenCapture` 接口不变，后续可替换为 VNC/PNG 承载。远程控制（键鼠注入）尚未实现。 |
| SQLite 索引（§9.3） | 自研 `LBS2` 二进制索引（相对路径 + 大小 + mtime + SHA-256 + 冲突标记 + 墓碑） | 环境无 SQLite；当前条目上限 20 万，按路径线性查找足够。规模增长后把 `SyncIndex` 存储层替换为 SQLite/SQLCipher 即可，不影响同步逻辑。 |
| libssh/SFTP 承载同步（§3.4） | 独立 STS 加密通道（`sync_port`，默认 45716）+ 自研清单/计划/块传输 | 与文件互传同源，复用 STS 双向认证与 AES-GCM 分帧；`SyncService` 接口不变，后续可换 SFTP 或基于版本库的增量协议。 |
| `.stignore` 文件（多行规则） | 配置项 `[sync].ignore` 逗号分隔字符串 | TOML 子集不支持数组；匹配语义保留 `*`（不跨 `/`）、`**`（跨层级，含零层级）、`?`、尾随 `/` 目录前缀、`!` 取反。 |
| 版本库 + 回收站（§9.5） | 自研 `VersionStore`：`<sync_root>/.linkbridge/versions/<yyyyMMdd-HHmmss-随机>/<原路径>`，覆盖/删除前归档，支持列表、恢复与三重上限清理 | 环境无 SQLite；按 `sync_root` 内目录组织便于按原路径查询与恢复，且随同步文件夹一起迁移。后续可换数据库记录元数据。 |
| 删除传播（§9.2） | 索引与清单携带墓碑（`deleted` + `deleted_at`）；计划用 `want_delete`（发起方删除）/`send_delete`（响应方自行删除）表达删除 | 墓碑避免“对端旧版本复活”；墓碑仅对曾同步过、磁盘确实不存在且未被忽略的路径生成，避免忽略/超限跳过被误判为删除。 |
| `max_concurrent_files` 并发传输（§9.4） | 已解析校验，实际仍为单会话串行传输；`rate_limit_kbps` 为发送端单文件平均限速 | 串行可保证协议状态简单、冲突判定确定；并发需把文件传输改为独立任务队列与多路复用，列入后续版本。 |
| 文件监听（§9.2） | Windows `ReadDirectoryChangesW`（OVERLAPPED + 250ms 等待）叠加 500ms 轮询与定时全量扫描 | 与设计一致；监听线程只置脏标记，业务线程防抖 1s 后执行，避免编辑半成品被传输；`ERROR_NOTIFY_ENUM_DIR` 时退化为全量扫描。 |
| CBOR 控制面编码 | 等价紧凑自定义二进制（大端、u32 长度前缀、64MB 上限） | 仅存在于传输边界，核心仅见解码字段，可整体替换为 CBOR。 |
| Android 加密（BoringSSL / NDK 原生） | 通过 JNI 调用 JCA（`MessageDigest`/`Mac`/`Cipher`/`Signature`/`KeyAgreement`），HKDF 为纯 C++ 基于 HMAC 实现 | Android 不带 OpenSSL 头文件与静态库；JCA 是系统内置实现，无需额外依赖。私钥以 PKCS#8 DER 不透明 blob 传递，公钥为 65 字节未压缩点；ECDSA 在 DER↔`r‖s` 间转换；ECDH 输出大端 32 字节 X，与 Windows `BCryptDeriveKey(RAW_SECRET)` 对齐，保证跨平台密钥一致。JCA 不支持从 EC 私钥反推公钥（`KeyFactory.getKeySpec(priv, ECPublicKeySpec.class)` 抛异常），因此生成密钥时把 `0x04‖X‖Y` 追加在 PKCS#8 DER 尾部，导入私钥前按尾部标记剥离、导出公钥时直接取尾部 65 字节。AES-GCM 的 AAD 必须经 `Cipher.updateAAD()` 注入（`update()` 会把 AAD 当明文/密文处理，导致与 Windows 的密文流不一致）；公钥坐标用 `BigInteger(byte[])` 构造（`BigInteger(String)` 只按十进制解析，不接受 `0x` 前缀）。 |
| Android Keystore 存私钥 | 私钥仍由核心以 PKCS#8 blob 持有，落盘时经 Kotlin `SecretStore`（AndroidKeyStore `linkbridge.master`，AES-256-GCM）加密；Keystore 只保护主密钥 | 核心 `crypto.h` 接口假定“私钥字节可用”，与 Keystore 不可导出密钥模型冲突。当前折中保留核心零改动；后续可增加 `ISecureStorage` 抽象，把私钥操作整体下沉到 Keystore。 |
| MediaProjection 抓屏（§4） | Kotlin `ScreenCaptureBridge`：`attach(MediaProjection, w, h, density)` 注册 ImageReader（RGBA_8888）+ VirtualDisplay，`captureDib()` 取最新帧转为 BGRA 并前置 40 字节 `BITMAPINFOHEADER` | 与 Windows 采集输出同一 `image/dib` 格式，核心与查看端（`latest.bmp`）零改动；`IScreenCapture` 接口不变。 |
| FileObserver（§4，API 29+） | Kotlin `SyncFileObserver` 对同步根目录 `walkTopDown()` 逐目录注册 `FileObserver`（跳过 `/.linkbridge`），变更通过 `nativeOnChange(handle)` 通知核心触发增量扫描 | FileObserver 不递归，必须逐目录注册；新增目录在下一次全量扫描时补注册。核心 `IFileWatcher` 接口不变。 |
| Android 剪贴板后台读取 | `ClipboardBridge` 在进程内注册 `OnPrimaryClipChangedListener` 并自增序号，核心按序号变化拉取文本 | Android 10+ 后台读取剪贴板受限，前台服务期间通常可读；无法读取时返回空并由核心跳过本轮，不报错。 |
| Android 路径与配置 | `LinkBridgeApp.onCreate` 注入 `filesDir`/`cacheDir`，配置为 `<filesDir>/config/linkbridge.toml`，首次启动自动把默认配置的 `sync.enabled=false` 改为 `true` 并写入 `<filesDir>/sync` | Android 无 `%APPDATA%` 且不允许环境变量；`platform.h` 增加 `android_set_paths`/`cache_dir()`，Windows 路径逻辑不变。 |
| Android 后台常驻（§3.3） | `LinkBridgeService`（`dataSync` 前台服务）承载 JNI 核心；`HeartbeatWorker` 以 15 分钟周期巡检，发现未运行则尝试拉起服务 | 与设计一致（WorkManager 周期下限 15 分钟 + 前台服务常驻）。停止服务时先 `cancel_all_confirms` 唤醒阻塞的确认请求，再销毁 Node，避免监听线程悬挂。 |

### A.3 平台实现现状

- Windows：CNG（SHA-256 / HMAC / HKDF / AES-GCM / ECDSA / ECDH）+ DPAPI + Winsock2 + GetAdaptersAddresses + Win32 Clipboard API（CF_UNICODETEXT / CF_DIB / CF_DIBV5，`WM_CLIPBOARDUPDATE` 监听，敏感标记过滤）+ GDI 屏幕采集（虚拟桌面 `BitBlt`，32 位 top-down DIB）+ `ReadDirectoryChangesW` 递归文件监听（OVERLAPPED）。已编译并通过全部单元/端到端测试。
- Android：`src/pal/android/` 提供 JNI 环境管理（`android_jni.cpp`：`JavaVM` 缓存、线程 attach/detach、类引用缓存）、路径注入（`android_paths.cpp`：`android_set_paths` / `primary_mac` / `local_ip_for`）、JCA 加密（`android_crypto.cpp`）、Keystore 保护的主密钥（Kotlin `SecretStore`）、剪贴板（Kotlin `ClipboardBridge`）、屏幕采集（Kotlin `ScreenCaptureBridge`，MediaProjection → DIB）与文件监听（Kotlin `SyncFileObserver` 递归注册 + `nativeOnChange`）。`android/` 为完整 Gradle 工程（Compose UI + `LinkBridgeService` + `ScreenShareService` + `HeartbeatWorker`），JNI 桥为 `lbcore_jni`。已在本地以 Gradle 9.4.1 + JDK 25 + AGP 8.5.2、SDK 34 / build-tools 34.0.0 / NDK 26.3.11579264 / CMake 3.22.1 执行 `gradle assembleDebug` 成功，产出 `android/app/build/outputs/apk/debug/app-debug.apk`，内含 `arm64-v8a` / `armeabi-v7a` / `x86_64` 三个 ABI 的 `liblbcore_jni.so` 与 `libc++_shared.so`。已在 MuMu 12 模拟器（Android 12 / SDK 32 / x86_64）实机运行：安装启动后 `Node::create`/`start()` 成功，生成 `identity.key`/`identity.name`/`linkbridge.toml`/`inbox`/`sync`，6 个业务端口（45710 配对、45711 心跳、45713 传输、45714 剪贴板、45715 屏幕、45716 同步）均已监听；`LinkBridgeService` 以前台服务（`isForeground=true`，通知渠道 `linkbridge_sync`）存活，Compose 主界面显示“服务运行中”、设备名、设备 ID 与指纹。随后与 Windows 控制台节点完成跨端联调：配对（UI 确认弹窗接受，Windows 端记录 `V2284A` 指纹）、Windows→Android 文件互传（`files/inbox/hello.txt` 内容一致 38 B）、剪贴板推送（确认后写入系统剪贴板，粘贴验证通过）、文件夹同步（含 `docs/` 子目录共 3 个文件 55 B，内容一致）、只读屏幕共享（12 秒接收 5 帧 1920×1080 BMP，非黑屏）均通过；`LinkBridgeService` 与 `ScreenShareService` 两个前台服务同时存活。MuMu 为 QEMU NAT（guest `wlan0=10.0.2.15`，宿主不可直连），联调经 `adb forward tcp:45710 tcp:45710` 建立通道，Windows 侧信任库记录对端 IP 为 `127.0.0.1`；因此 Android 主动发起（UI“立即同步/推送剪贴板”回连 Windows）与心跳在线状态需在真机或局域网环境补验。
- Linux：`src/pal/` 目前仅有 Windows 与 Android 实现；`crypto.h`、`platform.h`、`ByteStream`、`IDialer`、`IClipboardBackend`、`IScreenCapture`、`IFileWatcher` 已抽象，且 `src/net/tcp_posix.cpp` 已覆盖 POSIX socket，新增 `src/pal/linux/`（OpenSSL + Keyring + X11/Wayland 剪贴板 + PipeWire/X11 抓屏 + inotify）即可复用全部核心逻辑。

### A.4 已知限制

1. `pair` 命令以本机配置的 `pair_port` 作为对端端口，非对称端口场景需两端配置一致。
2. 心跳首次探测若对端尚未监听会记一次失败，下一次按基础间隔重试；成功后恢复自适应增长。
3. 身份私钥导出为 CNG ECCPRIVATE_BLOB 后经 DPAPI 加密存储；DPAPI 绑定当前 Windows 用户与主机，跨机迁移需重新配对。
4. 指纹确认当前由 CLI 人工完成，UI 客户端复用同一 `ConfirmFn` 接口。
5. 信任库落盘格式升级为 `LBT5`（新增对端文件夹同步端口），`PeerInfo` 升级为版本 2；旧格式不兼容，需重新配对。
6. 文件传输使用独立端口 `transfer_port`（默认 45713），剪贴板使用 `clipboard_port`（默认 45714），屏幕共享使用 `screen_port`（默认 45715），文件夹同步使用 `sync_port`（默认 45716），配对时随设备信息通告；SSH 端口 45712 仍为后续远程文件通道预留。
7. 续传仅按 `.lbpart` 文件长度确定前缀偏移，前缀内容不单独校验；最终整文件 SHA-256 校验兜底，失败会删除临时文件。
8. 对端 IP 变更后需重新配对：心跳探测不刷新信任库中的 `last_ip`，`send` 与剪贴板推送依赖配对时记录的地址与端口。
9. 剪贴板目前仅支持 UTF-8 文本与 Windows 原始 DIB 图片；不支持文件列表、HTML 等格式，跨平台图片互通需后续引入 PNG 编解码。
10. 本地剪贴板自动同步要求两端均运行 `daemon`；`clip push` 可直接推送，但要求信任库中已记录对端 IP（即至少完成过一次心跳或配对）。
11. 敏感内容过滤（`ExcludeClipboardContentFromMonitorProcessing`、`CanIncludeInClipboardHistory == 0`）仅在 Windows 生效；远端写入会覆盖本地剪贴板，`confirm` 模式无确认处理器时默认拒绝。
12. 同一台机器上同时运行两个节点时共享系统剪贴板，依赖哈希去重与 5s 回环窗口避免写入风暴；正常部署为不同主机，不受影响。
13. 屏幕共享为 v0 只读实现：协议无键鼠注入指令，`mode=control` 会被拒绝；后续远程控制需另立通道并补齐输入注入与“锁定输入”语义。
14. 控制台版本无 GUI 渲染，查看端仅把最新帧写为 `latest.bmp` 并输出统计；UI 客户端可直接订阅 `ScreenFrameReceived` 或读取该文件。
15. 帧格式为原始 32 位 DIB（非 PNG），单帧体积约为宽×高×4 字节；`[screen].max_bytes` 默认 16 MiB，约覆盖到 2560×1440，4K 桌面需调高至 64 MiB。`max_fps` 默认 2，避免带宽与 CPU 峰值。
16. 采集依赖交互式桌面：锁屏、UAC 安全桌面、无会话的服务上下文下 `BitBlt` 可能得到黑屏或失败；跨平台抓屏需新增 `IScreenCapture` 实现。
17. `screen view` 需要两端信任库已记录对端 `screen_port` 与 `last_ip`；信任库/配对信息由 `LBT4` 起携带屏幕端口，旧库需重新配对。
18. 仍为单文件夹实现：每端只同步 `[sync].folder` 指定的一个目录；`folder` 变更等同于重新建立索引，旧索引会被新一轮扫描覆盖。
19. 同步模式需两端一致（`send_receive` / `send_only` / `receive_only` / `mirror`）；握手时若模式不一致直接拒绝，避免“单方认为只收/只发”造成数据错乱。
20. 删除传播规则：`send_receive` 下，对端删除且本端未修改（哈希 == 基线）→ 跟随删除；本端已修改/重建 → 修改优先，复活对端。`send_only`/`mirror` 本端为权威源并传播本端删除；`receive_only` 跟随对端删除（本地改过的保留）。
21. 墓碑保留在索引中用于防止对端旧版本复活，当前不做墓碑回收；长期运行后索引会包含历史删除记录（清单上限 20 万条）。后续可增加“双方确认后清理”或按 `deleted_at` 过期清理。
22. 冲突策略三选一：`keep_both`（默认，冲突副本 `stem (conflict <设备> <UTC 时间戳>)ext`）、`newer`、`larger`；`newer/larger` 仅由响应方计算以避免双端分叉判定。冲突副本与原路径都会写入索引并标记 `conflicted`，后续会话跳过，直到人工解决。
23. 冲突解决为本地操作（CLI）：`keep_local` 归档并删除副本、`keep_remote` 归档原文件后用最新副本覆盖、`keep_both` 保留两个文件并清除标记。对端的冲突标记需在对端同样执行解决命令；当前没有跨端“已解决”同步协议。
24. 版本库/回收站：覆盖或删除前归档到 `<sync_root>/.linkbridge/versions/<id>/<原路径>`，恢复时当前文件再次归档；`trash_enabled=false` 时直接删除不归档。清理按 `version_keep_count`（每路径条数）、`version_keep_days`（天数）、`trash_max_mb`（总容量）三重上限，最旧优先。
25. 恢复历史版本视为本端修改：不更新同步基线，下一次同步会把恢复内容传播给对端；若对端已删除该路径，恢复会按“修改优先”复活文件。
26. 传输仍为串行：`max_concurrent_files` 已解析并校验但未启用并发；`rate_limit_kbps` 仅约束发送端单文件平均速率（每块发送后按已发送字节数补偿 sleep），接收端不限速。
27. 索引复用依赖文件大小与 mtime 命中，但 Windows 文件时间戳精度可能只有 1 秒。为避免“同一秒内等长改写”漏检，mtime 距今不足 2 秒的文件一律重新计算 SHA-256；这会带来少量重复哈希开销。
28. 验收中的“双端 10k 文件初始同步”与“长稳 72h”尚未做规模压测：单元/e2e 覆盖的是小规模目录；清单上限 20 万条，索引为线性查找，规模增长后需替换为 SQLite 并补压测。
29. Android 工程已在本地编译通过（Gradle 9.4.1 + SDK 34 + NDK 26.3 + CMake 3.22.1，`gradle assembleDebug`），并在 MuMu 12 模拟器（Android 12 / x86_64）完成启动验证与**跨端端到端联调**：服务可拉起、身份生成、端口监听、前台服务通知与 Compose 界面正常；与 Windows 控制台节点的配对、Windows→Android 文件互传、剪贴板推送、含子目录的文件夹同步、只读屏幕共享全部通过（结果见 A.3）。运行期修复了五处 Android 差异：Compose 状态订阅改用 `collectAsState()`（`collectAsStateWithLifecycle` 在某些无 `LocalLifecycleOwner` 的宿主上直接崩溃）、JCA 取 EC 公钥改用 `getAffineX()/getAffineY()`（`ECPoint.x/y` 为私有字段，JNI 访问触发 CheckJNI abort）、私钥 blob 尾部附带公钥、AES-GCM 的 AAD 改用 `updateAAD()`、公钥坐标改用 `BigInteger(byte[])`（后三处见 A.2）。联调拓扑注意：MuMu 使用 QEMU NAT，guest `wlan0=10.0.2.15` 对宿主不可直达，须在每个 adb 会话内重建 `adb forward tcp:<port> tcp:<port>`（adb daemon 重启会使映射失效）；由此 Windows 侧记录对端 IP 为 `127.0.0.1`，Android 主动发起的同步/剪贴板推送会在该拓扑下连到自身，需真机或局域网补验。仓库未提交 `gradlew`/`gradle-wrapper.jar`，`local.properties` 为机器相关文件（`sdk.dir`）；此外 Kotlin 1.9.24 在 JDK 25 下 Kotlin 增量编译守护进程启动失败，Gradle 自动回退为“不使用 Kotlin daemon”后编译成功，建议改用 JDK 17/21 或升级 Kotlin 版本以消除该告警。MuMu 多显示器环境下 App 窗口位于动态新增的虚拟 display，使用 `screencap`/`input` 时需显式指定 `-d <displayId>`，否则会截到/点到桌面启动器；MediaProjection 捕获到的是 display 0（启动器），因此共享画面内容与 App 所在虚拟 display 不同。
30. Android 剪贴板仅支持文本；Android 10+ 后台读取剪贴板受系统限制，需以前台服务运行且在应用可见时才稳定生效。`confirm` 模式下无确认处理器时仍默认拒绝，与桌面端一致。
31. Android 屏幕共享依赖 MediaProjection 用户授权：每次授权仅对单次 `getMediaProjection` 会话有效，进程/服务重启后需重新授权；无授权时 `capture()` 返回失败事件。Android 14+ 对 `mediaProjection` 前台服务类型有额外声明要求（已在 Manifest 声明）。
32. Android 同步目录固定为应用私有目录 `<filesDir>/sync`，无法直接选中 `/sdcard` 等外部共享目录；接入 SAF/MediaStore 需后续版本。`FileObserver` 不递归，新增子目录在下一次全量扫描时补注册，期间变更依赖定时扫描兜底。
33. Android 私钥未直接放入 Keystore：核心持有的 PKCS#8 私钥 blob 由 `SecretStore`（Keystore 主密钥 + AES-GCM）加密落盘，安全性依赖应用沙箱与 Keystore 不可导出主密钥；若需“私钥永不离开 TEE”，需新增 `ISecureStorage` 抽象。
34. 确认请求（屏幕共享/配对/`confirm` 剪贴板）在 Android 端经 `NativeBridge.onNativeEvent` 投递 `confirm_request`，UI 需在前台响应；60 秒无响应视为拒绝。服务停止时 `cancel_all_confirms` 统一拒绝所有在途确认，避免核心线程阻塞。
