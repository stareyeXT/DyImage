package hua.dy.image.feature.gallery

import android.content.ContentValues
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import hua.dy.image.utils.FileType
import hua.dy.image.utils.X2GifUtils
import splitties.init.appCtx
import java.io.File
import java.io.FileInputStream
import java.math.BigInteger
import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

internal fun isGifConvertible(path: String, fileType: FileType? = null): Boolean {
    if (path.endsWith(".gif", ignoreCase = true)) return false
    if (path.endsWith(".webp", ignoreCase = true)) return true
    if (path.endsWith(".heic", ignoreCase = true)) return true
    return fileType == FileType.WEBP || fileType == FileType.HEIC
}

// ---- GIF 缓存 ----

private val gifCacheDir: File by lazy {
    File(appCtx.externalCacheDir ?: appCtx.cacheDir, "image_share/gif_cache")
        .also { if (!it.exists()) it.mkdirs() }
        // 首次访问时清理超过 7 天的旧缓存文件
        .also { dir -> cleanupOldCache(dir) }
}

/** 清理超过 maxAgeDays 天的缓存文件 */
private fun cleanupOldCache(dir: File, maxAgeDays: Long = 7) {
    val deadline = System.currentTimeMillis() - maxAgeDays * 24 * 60 * 60 * 1000
    dir.listFiles()?.forEach { file ->
        if (file.lastModified() < deadline) file.delete()
    }
}

private fun computeFileMd5(file: File): String? = runCatching {
    MessageDigest.getInstance("MD5").let { md ->
        FileInputStream(file).use { input ->
            val buf = ByteArray(8192)
            var read: Int
            while (input.read(buf).also { read = it } != -1) {
                md.update(buf, 0, read)
            }
        }
        BigInteger(1, md.digest()).toString(16).padStart(32, '0')
    }
}.getOrNull()

private fun getCachedGif(sourcePath: String): String? {
    val source = File(sourcePath)
    if (!source.exists()) return null
    val md5 = computeFileMd5(source) ?: return null
    val cached = File(gifCacheDir, "$md5.gif")
    return if (cached.exists()) cached.absolutePath else null
}

private fun cacheGif(sourcePath: String, gifPath: String): String? {
    val md5 = computeFileMd5(File(sourcePath)) ?: return null
    val cached = File(gifCacheDir, "$md5.gif")
    if (!cached.exists()) {
        // 移动（renameTo）而非复制，省掉一次 I/O
        if (!File(gifPath).renameTo(cached)) return null
    } else {
        // 缓存已存在（并发场景），删除刚生成的文件
        File(gifPath).delete()
    }
    return cached.absolutePath
}

// ----------

internal fun convertToGif(sourcePath: String, nameSeed: String): String? {
    if (!isGifConvertible(sourcePath)) return null
    val source = File(sourcePath)
    if (!source.exists()) return null

    // 1) 命中缓存则直接返回
    getCachedGif(sourcePath)?.let { return it }

    val baseDir = appCtx.externalCacheDir ?: appCtx.cacheDir
    val outputDir = File(baseDir, "image_share/converted_gif")
    if (!outputDir.exists()) outputDir.mkdirs()

    val safeSeed = nameSeed.ifBlank { source.nameWithoutExtension.ifBlank { "image" } }
    val target = File(outputDir, "${safeSeed}_${System.currentTimeMillis()}.gif")
    val success = runCatching { X2GifUtils.convert(sourcePath, target.absolutePath) }.getOrDefault(false)
    if (!success) {
        if (target.exists()) target.delete()
        return null
    }

    // 2) 写入缓存（文件被 renameTo 到缓存目录）
    return cacheGif(sourcePath, target.absolutePath) ?: target.absolutePath
}

internal fun saveImageToLocal(path: String): Boolean {
    val source = File(path)
    if (!source.exists()) return false
    val rawExtension = source.extension.ifBlank { "png" }.lowercase(Locale.ROOT)
    val extension = if (rawExtension == "vvic") "heic" else rawExtension
    val mime = when (extension) {
        "jpg", "jpeg" -> "image/jpeg"
        "gif" -> "image/gif"
        "webp" -> "image/webp"
        "heic" -> "image/heic"
        else -> "image/png"
    }
    val fileName = "EImage_${SimpleDateFormat("yyyyMMdd_HHmmss", Locale.CHINA).format(Date())}.$extension"
    val values = ContentValues().apply {
        put(MediaStore.Images.Media.DISPLAY_NAME, fileName)
        put(MediaStore.Images.Media.MIME_TYPE, mime)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            put(MediaStore.Images.Media.RELATIVE_PATH, "${Environment.DIRECTORY_PICTURES}/EImage")
            put(MediaStore.Images.Media.IS_PENDING, 1)
        }
    }
    val resolver = appCtx.contentResolver
    val uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values) ?: return false
    val copied = runCatching {
        resolver.openOutputStream(uri)?.use { out ->
            source.inputStream().use { input ->
                input.copyTo(out)
            }
        } ?: return false
    }.isSuccess
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
        values.clear()
        values.put(MediaStore.Images.Media.IS_PENDING, 0)
        resolver.update(uri, values, null, null)
    }
    if (!copied) {
        resolver.delete(uri, null, null)
    }
    return copied
}

internal fun resolveSharePath(path: String): String {
    val source = File(path)
    if (!source.exists()) return path
    if (!source.extension.equals("vvic", ignoreCase = true)) return path

    val baseDir = appCtx.externalCacheDir ?: appCtx.cacheDir
    val targetDir = File(baseDir, "image_share/share_alias")
    if (!targetDir.exists()) targetDir.mkdirs()
    val target = File(targetDir, "${source.nameWithoutExtension}.heic")
    return runCatching {
        source.inputStream().use { input ->
            target.outputStream().use { output ->
                input.copyTo(output)
            }
        }
        target.absolutePath
    }.getOrDefault(path)
}
