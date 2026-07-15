package hua.dy.image.utils

import android.util.Log
import com.arthenica.ffmpegkit.FFmpegKit
import com.arthenica.ffmpegkit.ReturnCode
import java.io.File

object X2GifUtils {
    private const val TAG = "X2GifUtils"

    fun convert(rawPath: String, gifPath: String): Boolean {
        // WebP (including animated) -> use NDK path (libwebp + gif.h)
        return if (rawPath.endsWith(".webp", ignoreCase = true)) {
            val result = Webp2GifUtils.convert(webpPath = rawPath, gifPath = gifPath)
            Log.d(TAG, "WebP->GIF via NDK: result=$result")
            result
        } else {
            // HEIC and other formats -> use FFmpeg 2-pass palette for better quality
            convertWithPalette(rawPath, gifPath)
        }
    }

    /**
     * FFmpeg 2-pass conversion:
     * Pass 1: Generate optimized palette using ALL frames' pixels
     * Pass 2: Encode GIF using error-diffusion dithering for maximum quality
     */
    private fun convertWithPalette(rawPath: String, gifPath: String): Boolean {
        val palettePath = gifPath + ".palette.png"

        // Pass 1: Generate optimized palette
        // stats_mode=full: consider ALL pixels (not just changed pixels) → 更准确的色表
        // reserve_transparent=0: don't reserve a palette entry for transparency
        val paletteCmd = "-y -i \"$rawPath\" -vf \"palettegen=stats_mode=full:reserve_transparent=0\" \"$palettePath\""
        Log.d(TAG, "Pass 1: Generating palette")
        val paletteSession = FFmpegKit.execute(paletteCmd)

        if (!ReturnCode.isSuccess(paletteSession.returnCode)) {
            Log.w(TAG, "Palettegen failed, falling back to single-pass")
            return fallbackSinglePass(rawPath, gifPath)
        }

        // Pass 2: Encode GIF using the palette
        // dither=atkinson: Atkinson 误差扩散抖动，只传播 3/4 误差 → 比 Floyd-Steinberg
        // 更干净，孤立的噪点/像素点少得多，同时保留平滑过渡
        // 不使用 diff_mode=rectangle，避免矩形边界跳变导致的网格感
        // loop 0: 无限循环
        val encodeCmd = "-y -i \"$rawPath\" -i \"$palettePath\" -lavfi \"paletteuse=dither=atkinson\" -loop 0 \"$gifPath\""
        Log.d(TAG, "Pass 2: Encoding GIF")
        val encodeSession = FFmpegKit.execute(encodeCmd)

        // Clean up temp palette file
        runCatching { File(palettePath).delete() }

        val success = ReturnCode.isSuccess(encodeSession.returnCode)
        if (!success) {
            Log.e(TAG, "Encoding failed: ${encodeSession.output}")
            Log.e(TAG, "Logs: ${encodeSession.allLogsAsString}")
        }

        return success
    }

    private fun fallbackSinglePass(rawPath: String, gifPath: String): Boolean {
        Log.d(TAG, "Using single-pass fallback")
        val result = FFmpegKit.executeWithArguments(arrayOf("-y", "-i", rawPath, gifPath))
        return ReturnCode.isSuccess(result.returnCode)
    }
}
