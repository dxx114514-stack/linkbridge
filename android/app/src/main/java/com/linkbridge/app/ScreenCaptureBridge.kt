package com.linkbridge.app

import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.os.Handler
import android.os.Looper
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * MediaProjection 屏幕采集：输出 32 位 top-down DIB（BITMAPINFOHEADER + BGRA 像素），
 * 与 Windows 端 GDI 输出格式一致，核心层无需平台分支。
 */
object ScreenCaptureBridge {
    private var projection: MediaProjection? = null
    private var reader: ImageReader? = null
    private var display: VirtualDisplay? = null
    private var width = 0
    private var height = 0

    @JvmStatic
    fun attach(projection: MediaProjection, width: Int, height: Int, density: Int) {
        release()
        if (width <= 0 || height <= 0) return
        this.projection = projection
        this.width = width
        this.height = height

        projection.registerCallback(
            object : MediaProjection.Callback() {
                override fun onStop() {
                    release()
                }
            },
            Handler(Looper.getMainLooper()),
        )

        val r = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 2)
        reader = r
        display = projection.createVirtualDisplay(
            "LinkBridge",
            width,
            height,
            density,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            r.surface,
            null,
            Handler(Looper.getMainLooper()),
        )
    }

    @JvmStatic
    fun release() {
        try {
            display?.release()
        } catch (_: Exception) {
        }
        display = null
        try {
            reader?.close()
        } catch (_: Exception) {
        }
        reader = null
        try {
            projection?.stop()
        } catch (_: Exception) {
        }
        projection = null
        width = 0
        height = 0
    }

    @JvmStatic
    fun captureDib(): ByteArray? {
        val r = reader ?: return null
        val w = width
        val h = height
        if (w <= 0 || h <= 0) return null

        var image: android.media.Image? = null
        val deadline = System.currentTimeMillis() + 500
        while (image == null && System.currentTimeMillis() < deadline) {
            image = r.acquireLatestImage()
            if (image == null) {
                try {
                    Thread.sleep(30)
                } catch (_: InterruptedException) {
                    return null
                }
            }
        }
        val img = image ?: return null

        return try {
            val plane = img.planes[0]
            val buffer: ByteBuffer = plane.buffer
            val rowStride = plane.rowStride
            val pixelStride = plane.pixelStride
            if (pixelStride < 3 || buffer.capacity() < rowStride * (h - 1) + w * pixelStride) return null

            val pixels = ByteArray(w * h * 4)
            for (y in 0 until h) {
                val rowBase = y * rowStride
                var di = y * w * 4
                for (x in 0 until w) {
                    val si = rowBase + x * pixelStride
                    pixels[di] = buffer.get(si + 2) // B
                    pixels[di + 1] = buffer.get(si + 1) // G
                    pixels[di + 2] = buffer.get(si) // R
                    pixels[di + 3] = 0xFF.toByte()
                    di += 4
                }
            }

            val out = ByteArray(40 + pixels.size)
            val header = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN)
            header.putInt(40) // biSize
            header.putInt(w) // biWidth
            header.putInt(-h) // biHeight（负值 = top-down）
            header.putShort(1) // biPlanes
            header.putShort(32) // biBitCount
            header.putInt(0) // BI_RGB
            header.putInt(w * h * 4) // biSizeImage
            header.putInt(0) // biXPelsPerMeter
            header.putInt(0) // biYPelsPerMeter
            header.putInt(0) // biClrUsed
            header.putInt(0) // biClrImportant
            System.arraycopy(pixels, 0, out, 40, pixels.size)
            out
        } catch (_: Exception) {
            null
        } finally {
            try {
                img.close()
            } catch (_: Exception) {
            }
        }
    }
}
