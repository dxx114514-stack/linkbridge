package com.linkbridge.app

import android.app.Application
import android.content.Context

class LinkBridgeApp : Application() {
    override fun onCreate() {
        super.onCreate()
        appContext = this
        // 必须在任何 native 调用前注入应用私有目录
        LinkBridgeEngine.init(this)
    }

    companion object {
        /** 供通知镜像/悬浮球等后台组件使用，避免反复传 Context。 */
        @Volatile
        var appContext: Context? = null
            private set

        fun requireContext(): Context? = appContext
    }
}
