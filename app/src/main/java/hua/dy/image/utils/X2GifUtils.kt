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
     * Pass 1: Generate optimized palette
     * Pass 2: Encode GIF using the palette with high-quality dithering
     */
    private fun convertWithPalette(rawPath: String, gifPath: String): Boolean {
        val palettePath = gifPath + ".palette.png"

        // Pass 1: Generate optimized palette
        // stats_mode=diff: generate palette based on frame differences (better for animations)
        // reserve_transparent=0: don't reserve a palette entry for transparency
        val paletteCmd = "-y -i \"$rawPath\" -vf \"palettegen=stats_mode=diff:reserve_transparent=0\" \"$palettePath\""
        Log.d(TAG, "Pass 1: Generating palette")
        val paletteSession = FFmpegKit.execute(paletteCmd)

        if (!ReturnCode.isSuccess(paletteSession.returnCode)) {
            Log.w(TAG, "Palettegen failed, falling back to single-pass")
            return fallbackSinglePass(rawPath, gifPath)
        }

        // Pass 2: Encode GIF using the palette
        // dither=bayer:bayer_scale=5: Bayer dithering (fast, low noise)
        // diff_mode=rectangle: only update changed regions (smaller file)
        // loop 0: infinite loop
        val encodeCmd = "-y -i \"$rawPath\" -i \"$palettePath\" -lavfi \"paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle\" -loop 0 \"$gifPath\""
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
