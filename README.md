# 连桥 LinkBridge

局域网内的跨设备协同工具：**手机与电脑在同一网络下**即可互传文件、同步剪贴板、镜像通知、共享屏幕、同步文件夹，无需云服务、无需公网端口映射。

- 核心为 C++20 静态库 `lb_core`，Windows / Linux / Android 复用同一套协议与业务逻辑
- 三种前端形态：命令行 `linkbridge`、图形界面 `linkbridge-gui`（wxWidgets）、Android App（Kotlin + Compose + JNI）
- 设备间使用 **ECDH 密钥协商 + HKDF 派生 + AEAD 加密**，配对码参与密钥派生，会话与传输全程加密

## 功能特性

| 模块 | 说明 | 状态 |
| --- | --- | --- |
| 身份与信任库 | 设备身份、SSH 公钥、指纹校验、受信任设备持久化 | M1 |
| 配对 | 6 位配对码 + 指纹核对，两端确认后建立信任 | M2 |
| 心跳/在线状态 | 局域网内设备发现与在线/离线事件 | M3 |
| 文件互传 | 点对点传输，进度与完成事件、失败原因回传 | M4 |
| 剪贴板同步 | 文本/图像推送，防回环，自动获取（Android 回到前台时读取） | M5 |
| 屏幕共享 | 只读查看指定受信任设备屏幕（Android 需授权 MediaProjection） | M6 |
| 文件夹同步 | 索引、增量同步、冲突检测与解决、历史版本/回收站 | M7 |
| 通知镜像 | Android 通知监听转发到对端（含原始通知动作唤起） | M8 |
| 细粒度设备权限 | 按设备开关各项能力（同步/传输/剪贴板/屏幕/通知等），即时生效并记录审计日志 | 增强 |
| 悬浮球 | 可自定义：单击功能轮盘、双击快捷剪贴板同步、吸边隐藏 | Android |
| Shizuku | 借助 Shizuku 执行需要提权的自动化操作 | Android |

## 架构

```
src/core/   业务核心（不 include 任何 socket 头文件，网络能力经接口注入）
  common/ config/ crypto/ identity/ trust/ pairing/ heartbeat/
  transfer/ clipboard/ screen/ sync/ notify/ node/ platform/
src/net/    加密分帧（secure_channel）与 TCP 实现（Windows / POSIX）
src/pal/    平台适配层：Windows(CNG/DPAPI/X11) / Linux(OpenSSL/X11/inotify) / Android(JNI/Keystore)
src/app/    命令行入口
src/gui/    wxWidgets 图形界面
android/    Android 工程（Compose UI + NativeBridge(JNI) + 前台服务）
tests/      单元测试与端到端测试
docs/       设计文档与项目站点
```

`lb::Node` 组合各服务（配对、心跳、传输、剪贴板、屏幕、同步、通知、信任库），
Android 通过 `lb_jni.cpp` 暴露为 `NativeBridge`，事件以 JSON 回传 `onNativeEvent`。

## 端口约定

| 端口 | 用途 |
| --- | --- |
| 45710 | 配对 |
| 45711 | 心跳 |
| 45712 | SSH |
| 45713 | 文件传输 |
| 45714 | 剪贴板 |
| 45715 | 屏幕共享 |
| 45716 | 文件夹同步 |
| 45717 | 通知镜像 |

端口可在 `linkbridge.toml` 的 `[network]` 段修改。**同一台机器上不要同时运行 GUI 与 CLI**（两者都会监听上述端口，可能争抢连接）。

## 构建（Windows / Linux）

依赖：CMake ≥ 3.20、支持 C++20 的编译器、OpenSSL（Linux）、wxWidgets（可选，用于图形界面）。

```bash
# 仅核心 + CLI + 测试（不构建 GUI）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLB_BUILD_GUI=OFF
cmake --build build --parallel

# 构建图形界面（需已安装 wxWidgets）
cmake -S . -B build-gui -DCMAKE_BUILD_TYPE=Release
cmake --build build-gui --parallel
```

产物：`build/linkbridge(.exe)`、`build-gui/linkbridge-gui(.exe)`、`build/lb_tests`、`build/lb_e2e`。

Windows 下若使用 MinGW/MSYS2，可加 `-G Ninja`。Linux 若缺少开发包头文件，可通过 `-DLB_SYSROOT=<目录>` 指定自行解包的系统开发包。

运行测试：

```bash
ctest --test-dir build --output-on-failure
```

## 快速开始（命令行）

```bash
# 初始化身份与配置（默认数据目录：Windows 为 %LOCALAPPDATA%\LinkBridge）
linkbridge init

# 查看本机身份、配置与受信任设备
linkbridge show

# A 机：生成配对码并等待对端配对（默认 300 秒，最多 3 次尝试）
linkbridge code

# B 机：用 A 机显示的 IP 与配对码发起配对，两端各自确认指纹
linkbridge pair <对端IP> <配对码>

# 启动常驻服务（监听/心跳/剪贴板/屏幕/同步/通知）
linkbridge daemon
```

常用命令：

```bash
linkbridge devices                                  # 列出受信任设备
linkbridge send <设备> <文件> [远端文件名]            # 发送文件
linkbridge clip status | get                        # 剪贴板配置 / 读取本地剪贴板
linkbridge clip push <设备|all> <文本>                # 推送文本
linkbridge screen status | view <设备> [输出目录]     # 屏幕共享（只读，最新帧保存为 BMP）
linkbridge sync status | once <设备|all>            # 文件夹同步
linkbridge sync conflicts                           # 未解决冲突
linkbridge sync resolve <路径> <keep_local|keep_remote|keep_both>
linkbridge sync history [路径] / restore <路径> [版本ID]
linkbridge perms [设备] | perms <设备> <权限> <on|off> | perms log
linkbridge notify status
```

通用参数：`--data-dir DIR`、`--config FILE`、`--name NAME`、`--auto-accept`（跳过人工确认，仅建议测试用）、`--duration N`。
完整说明见 `linkbridge`（无参数运行）或 `docs/clidoc.html`。

图形界面：直接运行 `linkbridge-gui`，参数与 CLI 一致。

## 配对流程

1. 任一端生成配对码（CLI `linkbridge code` / GUI「生成配对码」/ 手机「生成配对码」），配对码 6 位、默认 300 秒有效、最多 3 次尝试；
2. 另一端输入对端 IP 与配对码发起配对（手机：主界面「向对端配对」）；
3. 两端弹出「配对请求 + 指纹」，人工核对指纹一致后确认；
4. 成功后写入受信任设备列表。之后可在「受信任设备」中按需调整每台设备的权限。

配对失败常见原因：

- 对端没有生成有效配对码，或配对码已过期/尝试次数用尽；
- 两端不在同一网段，或被防火墙拦截（需放行 45710 端口）；
- 本机服务未启动（手机端会在配对时自动拉起前台服务）；
- 同一台电脑上 GUI 与 CLI 同时运行，连接被另一个进程接收。

## Android 构建与安装

环境：Android SDK（compileSdk 34）、NDK、CMake、Gradle（项目未包含 wrapper，可用本机 Gradle 9.x）。

```bash
cd android
gradle :app:assembleDebug
# 产物: android/app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

- `minSdk 26` / `targetSdk 34`，ABI：`arm64-v8a`、`armeabi-v7a`、`x86_64`
- 首次使用需授予：通知权限、通知监听（通知镜像）、悬浮窗（悬浮球）、无障碍（输入注入）、MediaProjection（屏幕共享）
- 悬浮球：单击打开功能轮盘（同步剪贴板 / 打开连桥 / 停止或启动服务 / 隐藏悬浮球），双击直达「读取剪贴板 → 同步 → 退出」快捷流程
- 剪贴板自动获取：App 回到前台时读取当前剪贴板，内容变化才推送（Android 10+ 限制后台读取剪贴板）
- Android 私钥保存在 Android Keystore；Windows 使用 CNG/DPAPI；Linux 使用系统密钥存储

## CI 与发布

- `.github/workflows/ci.yml`：Windows（MinGW/MSYS2）与 Linux 上构建并运行 `ctest`
- 推送到 `main` 且提交标题为 `build: X.Y.Z` 时，自动创建 `vX.Y.Z` Release，正文为自上一个 Release 以来的提交记录

## 文档

- 设计与协议细节：[docs/LinkBridge设计文档.md](docs/LinkBridge设计文档.md)
- 项目站点 / CLI 文档：[docs/index.html](docs/index.html)、[docs/clidoc.html](docs/clidoc.html)
- 支持与帮助：[docs/support.html](docs/support.html)

## 安全说明

- 配对码只用于派生初始密钥，最终信任依靠 ECDH 协商 + 指纹核对（指纹基于两端设备 ID 与公钥排序计算，防中间人）
- 传输采用 AEAD 加密并带序号防重放；密钥由 HKDF-SHA256 派生
- 权限按设备独立存储（位掩码），变更即时生效并写入审计日志（`linkbridge perms log`）
- 所有能力仅在同局域网、已配对设备之间可用，不依赖云端
