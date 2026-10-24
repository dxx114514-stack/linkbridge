package com.linkbridge.app

import android.os.FileObserver
import java.io.File
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList

/**
 * 递归文件监听：为同步根目录下每个子目录注册 FileObserver，
 * 任一事件回调 C++ 侧的 nativeOnChange(handle)，由核心做 500ms 防抖与增量扫描。
 */
object SyncFileObserver {
    private const val MASK =
        FileObserver.CREATE or FileObserver.DELETE or FileObserver.MODIFY or
            FileObserver.MOVED_FROM or FileObserver.MOVED_TO or FileObserver.ATTRIB or
            FileObserver.DELETE_SELF or FileObserver.MOVE_SELF or FileObserver.CLOSE_WRITE

    private val watchers = ConcurrentHashMap<Long, CopyOnWriteArrayList<FileObserver>>()

    @JvmStatic
    fun start(root: String, handle: Long): Boolean {
        val dir = File(root)
        if (!dir.isDirectory) return false
        stop(handle)

        val list = CopyOnWriteArrayList<FileObserver>()
        dir.walkTopDown()
            .filter { it.isDirectory && !it.path.contains("/.linkbridge") }
            .forEach { folder ->
                val observer = object : FileObserver(folder.absolutePath, MASK) {
                    override fun onEvent(event: Int, path: String?) {
                        nativeOnChange(handle)
                    }
                }
                observer.startWatching()
                list.add(observer)
            }
        watchers[handle] = list
        return true
    }

    @JvmStatic
    fun stop(handle: Long) {
        watchers.remove(handle)?.forEach {
            try {
                it.stopWatching()
            } catch (_: Exception) {
            }
        }
    }

    /** 由 C++ android_watcher.cpp 实现；句柄为原生 WatcherContext 指针。 */
    @JvmStatic
    external fun nativeOnChange(handle: Long)
}
