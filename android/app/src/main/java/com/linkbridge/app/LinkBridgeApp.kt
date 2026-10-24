package com.linkbridge.app

import android.app.Application

class LinkBridgeApp : Application() {
    override fun onCreate() {
        super.onCreate()
        // 必须在任何 native 调用前注入应用私有目录
        LinkBridgeEngine.init(this)
    }
}
