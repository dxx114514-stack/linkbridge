# JNI 入口与桥接类需要保留（release 未启用 minify，此处仅为将来开启 R8 时兜底）
-keep class com.linkbridge.app.NativeBridge { *; }
-keep class com.linkbridge.app.SecretStore { *; }
-keep class com.linkbridge.app.ClipboardBridge { *; }
-keep class com.linkbridge.app.ScreenCaptureBridge { *; }
-keep class com.linkbridge.app.SyncFileObserver { *; }
