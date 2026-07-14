package hua.dy.image.utils

import android.util.Log
import com.arthenica.ffmpegkit.FFmpegKit
import com.arthenica.ffmpegkit.ReturnCode

object X2GifUtils {
    private const val TAG = "X2GifUtils"

    fun convert(rawPath: String, gifPath: String): Boolean {
        // WebP (including animated) -> use NDK path (libwebp + gif.h)
        // ffmpeg-kit-min does NOT support WebP animation chunks (ANMF)
        return if (rawPath.endsWith(".webp", ignoreCase = true)) {
            val result = Webp2GifUtils.convert(webpPath = rawPath, gifPath = gifPath)
            Log.d(TAG, "WebP->GIF via NDK: result=$result")
            result
        } else {
            // HEIC and other formats -> use FFmpeg
            val result = FFmpegKit.executeWithArguments(
                arrayOf(
                    "-y",
                    "-i",
                    rawPath,
                    gifPath
                )
            )
            val success = ReturnCode.isSuccess(result.returnCode)
            Log.d(TAG, "FFmpeg convert: returnCode=${result.returnCode}, success=$success")
            if (!success) {
                Log.d(TAG, "FFmpeg logs: ${result.allLogsAsString}")
            }
            success
        }
    }
}
