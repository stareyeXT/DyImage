# 计划：WebP→GIF 转换全面优化

## 目标
全面提升 EImage 中 WebP（及 HEIC）动图转 GIF 的质量、性能、文件大小和用户体验。

## 涉及文件

### C++ 层（核心转换器）
| 文件 | 改动量 | 说明 |
|---|---|---|
| `app/src/main/cpp/webp2gif.cpp` | 大改 | 核心转换逻辑重构 |
| `app/src/main/cpp/gif.h` | 中改 | 新增功能支持 |
| `app/src/main/cpp/webp2gif.h` | 不改 | API 不变 |
| `app/src/main/cpp/jni_bridge.cpp` | 不改 | API 不变 |

### Kotlin 层
| 文件 | 改动量 | 说明 |
|---|---|---|
| `app/.../GalleryFileOps.kt` | 小改 | 添加缓存逻辑 |
| `app/.../X2GifUtils.kt` | 小改 | 添加缓存检查 |

---

## 任务分解

### 任务 1：多帧联合调色板采样（质量）

**文件**：`webp2gif.cpp`

**现状**：调色板只从第 1 帧采样生成。后段颜色不同的帧量化质量下降。

**方案**：
- 在 `webp2gif()` 中，先遍历前 `N` 帧（或全部帧按比例采样），收集像素到一个大采样池
- 采样策略：每帧均匀采样（如每帧取 10% 像素），避免内存爆炸
- 用这个多帧采样池生成一个全局共享调色板
- 首帧过完后不再额外遍历——解码阶段同时采样

**伪代码变更**：
```cpp
// 新增：多帧采样缓冲
struct PaletteSampler {
    static constexpr int MAX_SAMPLES = 256 * 1024;  // 256K 像素 ≈ 1MB
    uint8_t samples[MAX_SAMPLES * 4];
    int count = 0;
    
    void addFrame(const uint8_t* frame, int numPixels, int stride = 8) {
        for (int i = 0; i < numPixels && count < MAX_SAMPLES; i += stride) {
            memcpy(samples + count * 4, frame + i * 4, 4);
            count++;
        }
    }
    
    void buildPalette(GifPalette* pal, int bitDepth) {
        GifMakePalette(nullptr, samples, /*width*/ count, /*height*/ 1, bitDepth, true, pal);
    }
};
```

**效果**：对多场景切换的动画，颜色准确度显著提升。

---

### 任务 2：差异矩形 + Disposal 优化（文件大小）

**文件**：`gif.h` + `webp2gif.cpp`

**现状**：每帧写入全画布 `(0,0,width,height)`，disposal 固定为 `0x05`（不恢复+透明）。即使只有几个像素变化，也编码整帧。

**方案**：

**2a. 在 gif.h 中新增差异矩形计算**

新增函数：
```cpp
// 计算两帧之间的差异矩形（bounding box of changed pixels）
bool GifComputeDiffRect(const uint8_t* prev, const uint8_t* curr, 
                        int width, int height, 
                        int& left, int& top, int& right, int& bottom);
```

返回 `false` 表示无差异（可跳过此帧）。每 8×8 块粗检测，再精确到像素。

**2b. 在 gif.h 中新增 disposal 参数**

修改 `GifWriteLzwImage` 签名：
```cpp
// 新增 disposal 参数：0=无指定, 1=不恢复, 2=恢复到背景色, 3=恢复到前一帧
void GifWriteLzwImage(FILE* f, uint8_t* image, 
                      uint32_t left, uint32_t top, uint32_t width, uint32_t height,
                      uint32_t delay, GifPalette* pPal, 
                      int disposalMethod = 1);  // 默认 1 = 不恢复
```

GCE 字节从 `0x05` 改为 `0x04 | disposalMethod << 2`（保留透明标志）。

**2c. 在 webp2gif.cpp 中应用优化**

```cpp
// 对非首帧：计算与 oldImage 的差异矩形
int diffLeft, diffTop, diffRight, diffBottom;
bool hasDiff = GifComputeDiffRect(writer.oldImage, frame_data, 
                                   width, height, 
                                   diffLeft, diffTop, diffRight, diffBottom);
if (!hasDiff) {
    // 帧无变化：只需延长上一帧的显示时间（合并延迟）
    // 跳过写入，将 delay 累加到下一帧
    accumulatedDelay += delay;
    continue;
}

// 根据差异矩形大小决定策略
int diffW = diffRight - diffLeft;
int diffH = diffBottom - diffTop;
float changeRatio = (diffW * diffH) / (float)(width * height);

if (changeRatio < 0.3f) {
    // 小区域变化：只写入差异部分 + disposal=1（不恢复背景）
    // 先复制差异区域的像素到新缓冲区
    // 用 disposal=1 写入（下一帧会覆盖该区域）
    GifWriteLzwImage(f, buf, diffLeft, diffTop, diffW, diffH, delay, &pal, 1);
} else {
    // 大面积变化：写入全帧 + disposal=2（恢复到背景色）
    // 避免残留 artifacts
    GifWriteLzwImage(f, buf, 0, 0, width, height, delay, &pal, 2);
}
```

**特别处理**：
- 首帧 always 全帧 + disposal=2
- 当变化面积 > 30% 时，全帧写入避免复杂计算
- 累计延迟（多个无变化帧合并）
- 若 `prev_ts == curr_ts`（零延迟帧），合并到上一帧

**效果**：对局部动画（如表情包眨眼），文件体积预计减少 **40-60%**。

---

### 任务 3：蛇形扫描抖动（质量）

**文件**：`gif.h`

**现状**：`GifDitherImage` 固定从左到右、从上到下传播误差，产生方向性条纹。

**方案**：将 Floyd-Steinberg 改为蛇形扫描：
```cpp
void GifDitherImage(..., bool serpentine = true) {
    for (uint32_t yy = 0; yy < height; ++yy) {
        bool leftToRight = !serpentine || (yy % 2 == 0);
        if (leftToRight) {
            for (uint32_t xx = 0; xx < width; ++xx) {
                // 标准方向：误差向右、向下传播
                ditherPixel(..., xx, yy, +1);
            }
        } else {
            for (uint32_t xx = width; xx > 0; --xx) {
                // 反向：误差向左、向下传播
                ditherPixel(..., xx - 1, yy, -1);
            }
        }
    }
}
```

需要修改误差传播方向的左右反转逻辑。

**效果**：消除方向性条纹，抖动噪声更接近白噪声，视觉更自然。

---

### 任务 4：直接查表法调色板搜索（性能）

**文件**：`gif.h`

**现状**：`GifGetClosestPaletteColor` 走 k-d 树搜索，每像素 O(log 256) ≈ 8 次比较。被原库注释标记为 "major hotspot"。

**方案**：
```cpp
// 在 GifMakePalette/gif 初始化后，建立 5-5-5 RGB 直接映射表
// 32768 种输入 → 1 字节调色板索引
uint8_t gifLookupTable[32768];

void GifBuildLookupTable(GifPalette* pPal) {
    for (int r = 0; r < 32; ++r) {
        for (int g = 0; g < 32; ++g) {
            for (int b = 0; b < 32; ++b) {
                int r5 = (r << 3) | (r >> 2);
                int g5 = (g << 3) | (g >> 2);
                int b5 = (b << 3) | (b >> 2);
                int bestDiff = INT_MAX, bestInd = 0;
                GifGetClosestPaletteColor(pPal, r5, g5, b5, &bestInd, &bestDiff, 1);
                int idx = (r << 10) | (g << 5) | b;
                gifLookupTable[idx] = (uint8_t)bestInd;
            }
        }
    }
}

// 在抖动/阈值化中用查表替代 k-d 树搜索
// 查找：int idx = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
// uint8_t bestInd = gifLookupTable[idx];
```

查表仅需在调色板确定后执行一次（32K 次搜索），但后续每帧的每像素搜索变为 O(1) 数组访问。

**效果**：量化阶段速度提升 **5-10 倍**，对高帧数动画效果显著。

---

### 任务 5：转换结果缓存（用户体验）

**文件**：`GalleryFileOps.kt` + `X2GifUtils.kt`

**现状**：每次点击「转换为 GIF」都重新编码。同一文件多次分享反复转换。

**方案**：

**5a. 在 `GalleryFileOps.kt` 中新增缓存逻辑**

```kotlin
private val gifCacheDir: File by lazy {
    File(appCtx.externalCacheDir ?: appCtx.cacheDir, "image_share/gif_cache")
        .also { if (!it.exists()) it.mkdirs() }
}

internal fun getCachedGif(sourcePath: String): String? {
    val source = File(sourcePath)
    if (!source.exists()) return null
    // 使用源文件 MD5 作为缓存键
    val md5 = computeFileMd5(source) ?: return null
    val cached = File(gifCacheDir, "$md5.gif")
    return if (cached.exists()) cached.absolutePath else null
}

internal fun cacheGif(sourcePath: String, gifPath: String): Boolean {
    val source = File(sourcePath)
    val md5 = computeFileMd5(source) ?: return false
    val cached = File(gifCacheDir, "$md5.gif")
    return File(gifPath).renameTo(cached) || // 移动而非复制
           gifPath == cached.absolutePath
}

private fun computeFileMd5(file: File): String? {
    // 快速 MD5（只读前 4KB + 后 4KB + 文件大小，平衡速度与唯一性）
    // 或全量 MD5（更安全，用 MessageDigest）
    return runCatching {
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
}
```

**5b. 修改 `convertToGif()`**：
```kotlin
internal fun convertToGif(sourcePath: String, nameSeed: String): String? {
    // 1. 查缓存
    getCachedGif(sourcePath)?.let { return it }
    
    // 2. 原转换逻辑
    // ...
    val success = X2GifUtils.convert(sourcePath, target.absolutePath)
    if (!success) return null
    
    // 3. 写入缓存
    cacheGif(sourcePath, target.absolutePath)
    return target.absolutePath
}
```

**5c. 缓存清理策略**（可选，可加在 `AppSettingsStore` 或设置页）：
- 启动时清理超过 7 天的缓存
- 或简单 LRU：缓存目录超过 200MB 时清理最旧文件

**效果**：同一文件第二次转换秒级响应，零等待。

---

## 实现顺序

| 步骤 | 任务 | 依赖 | 预计工时 |
|---|---|---|---|
| 1 | **任务 4**：直接查表法（纯加法，不影响其他逻辑） | 无 | ~30min |
| 2 | **任务 3**：蛇形扫描抖动（纯替换） | 无 | ~20min |
| 3 | **任务 2**：差异矩形 + disposal 优化 | 无 | ~1.5h |
| 4 | **任务 1**：多帧联合调色板（依赖任务 2 的差值比较逻辑） | 任务 2 | ~1h |
| 5 | **任务 5**：转换缓存（纯 Kotlin 侧） | 无 | ~30min |
| 6 | 集成测试：验证各文件格式、边界条件 | 全部 | ~1h |

**总预计**：~5 小时

---

## 测试清单

- [ ] 纯色简单动画（颜色无变化 → 跳过帧合并）
- [ ] 局部变化动画（眨眼/打字 → 差异矩形生效）
- [ ] 全景变化动画（场景切换 → 回退全帧）
- [ ] 带透明通道 WebP（Alpha 合成验证）
- [ ] 多帧调色板覆盖测试（首帧红色 → 后段蓝色）
- [ ] 缓存命中/未命中测试
- [ ] 缓存过期清理测试
- [ ] 兼容性：Android 7-15
- [ ] 大文件压力测试（100+ 帧）
- [ ] 多选批量转换验证
- [ ] 与 FFmpeg HEIC→GIF 路径的接口兼容性
