#include "webp2gif.h"
#include "gif.h"
#include "webp/demux.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <android/log.h>

#define LOG_TAG "Webp2Gif"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// 采样四角小区域，用"众数"猜测背景色（抗噪声，避免单像素内容污染）
static void guessBackgroundFromCorners(uint8_t* frame, int width, int height, uint8_t& bgR, uint8_t& bgG, uint8_t& bgB) {
    // 图片太小时无法安全采样，直接返回黑色背景
    if (width < 8 || height < 8) { bgR = 0; bgG = 0; bgB = 0; return; }
    // 在每个角取一个 patch（边长 = min(width,height)/8，至少 4 像素），统计出现最多的颜色
    int patch = std::min(width, height) / 8;
    if (patch < 4) patch = 4;
    if (patch > 32) patch = 32;

    auto pixelAt = [&](int x, int y) -> uint8_t* {
        return frame + (y * width + x) * 4;
    };

    // 用粗直方图（把 RGB 各量化到 5 bit）求众数，对轻微压缩噪声更稳
    int hist[32][32][32] = {};
    int xs[] = {0, width - patch, 0, width - patch};
    int ys[] = {0, 0, height - patch, height - patch};
    for (int c = 0; c < 4; ++c) {
        for (int y = 0; y < patch; ++y) {
            for (int x = 0; x < patch; ++x) {
                uint8_t* p = pixelAt(xs[c] + x, ys[c] + y);
                int r = p[0] >> 3, g = p[1] >> 3, b = p[2] >> 3;
                ++hist[r][g][b];
            }
        }
    }
    int best = 0, br = 0, bg = 0, bb = 0;
    for (int r = 0; r < 32; ++r)
        for (int g = 0; g < 32; ++g)
            for (int b = 0; b < 32; ++b)
                if (hist[r][g][b] > best) { best = hist[r][g][b]; br = r; bg = g; bb = b; }
    bgR = (uint8_t)((br << 3) | (br >> 2));
    bgG = (uint8_t)((bg << 3) | (bg >> 2));
    bgB = (uint8_t)((bb << 3) | (bb >> 2));
}

// A2：正确的预合成管线。
// 1) 优先用 WebPAnimInfo.bgfx（ARGB 格式，真实画布背景）作为合成背景；alpha<255 时 fallback 到四角众数法
// 2) 对非纯背景像素统一执行 over 合成。不设 ALPHA_CEIL 跳过——边缘抗锯齿像素（alpha≈250-254）
//    原本针对透明背景混合，保留原色不合成到真实背景上会导致边缘偏亮，量化后产生白色锯齿。
// 3) 所有合成后的像素 alpha 置 255，消除 GIF 色表量化前的半透明干扰。
static void compositeOntoBackground(uint8_t* frame, int numPixels, uint8_t bgR, uint8_t bgG, uint8_t bgB) {
    const int ALPHA_FLOOR = 6;      // 低于此 alpha 视为纯背景（去掉半幽灵边缘）
    for (int i = 0; i < numPixels * 4; i += 4) {
        uint8_t a = frame[i + 3];
        if (a == 255) continue;                     // 已不透明，无需合成
        if (a <= ALPHA_FLOOR) {                     // 纯背景像素
            frame[i + 0] = bgR;
            frame[i + 1] = bgG;
            frame[i + 2] = bgB;
            frame[i + 3] = 255;
            continue;
        }
        // 整数 over 合成：result = (fg * a + bg * (255-a) + 128) / 255
        // 避免 float 运算，alpha=255 时 R*bg=0 → result≈R（但已提前 continue）
        int invA = 255 - a;
        frame[i + 0] = (uint8_t)((frame[i + 0] * a + bgR * invA + 128) / 255);
        frame[i + 1] = (uint8_t)((frame[i + 1] * a + bgG * invA + 128) / 255);
        frame[i + 2] = (uint8_t)((frame[i + 2] * a + bgB * invA + 128) / 255);
        frame[i + 3] = 255;
    }
}

// 多帧联合调色板采样器
// 从多个帧均匀采集像素样本，生成更准确的全局调色板
struct PaletteSampler {
    static const int MAX_SAMPLES = 256 * 1024;  // 约 1MB 采样缓冲
    uint8_t* samples;  // 堆分配，避免 1MB 压栈
    int count;
    int stride;
    
    PaletteSampler(int numPixels, int frameCount)
        : samples(new uint8_t[MAX_SAMPLES * 4]), count(0), stride(0) {
        // 计算采样步长：保证在 frameCount 帧内收集到足够样本
        // 用 int64_t 防止 numPixels * frameCount 溢出
        int64_t totalPixels = (int64_t)numPixels * frameCount;
        stride = (int)(totalPixels / MAX_SAMPLES + 1);
        if (stride < 1) stride = 1;
        // 但至少隔 2 像素采样，避免连续帧颜色冗余
        if (stride < 2) stride = 2;
    }
    
    ~PaletteSampler() { delete[] samples; }
    // 禁止拷贝（持有堆指针）
    PaletteSampler(const PaletteSampler&) = delete;
    PaletteSampler& operator=(const PaletteSampler&) = delete;
    // 允许移动
    PaletteSampler(PaletteSampler&& other) noexcept
        : samples(other.samples), count(other.count), stride(other.stride) {
        other.samples = nullptr;
    }
    PaletteSampler& operator=(PaletteSampler&& other) noexcept {
        if (this != &other) {
            delete[] samples;
            samples = other.samples;
            count = other.count;
            stride = other.stride;
            other.samples = nullptr;
        }
        return *this;
    }
    
    // 从帧数据中添加采样像素
    void addFrame(const uint8_t* frame, int numPixels) {
        if (count >= MAX_SAMPLES) return;
        for (int i = 0; i < numPixels && count < MAX_SAMPLES; i += stride) {
            memcpy(samples + count * 4, frame + i * 4, 4);
            count++;
        }
    }
    
    bool hasEnough() const { return count >= MAX_SAMPLES; }
    int sampleCount() const { return count; }
    
    // 从收集的样本生成调色板
    void buildPalette(GifPalette* pal) const {
        GifMakePalette(nullptr, samples, count, 1, 8, true, pal);
        LOGD("多帧调色板已生成: 采样数=%d", count);
    }
};

bool webp2gif(const std::string& webp_path, const std::string& gif_path) {
    // 打开 WebP 文件
    FILE* webp_file = fopen(webp_path.c_str(), "rb");
    if (!webp_file) {
        LOGE("打开 WebP 失败: %s", webp_path.c_str());
        return false;
    }

    // 获取文件大小
    fseek(webp_file, 0, SEEK_END);
    size_t file_size = ftell(webp_file);
    fseek(webp_file, 0, SEEK_SET);

    // 读取文件数据
    uint8_t* webp_data = (uint8_t*)malloc(file_size);
    if (!webp_data) {
        LOGE("内存分配失败");
        fclose(webp_file);
        return false;
    }
    size_t read_bytes = fread(webp_data, 1, file_size, webp_file);
    fclose(webp_file);
    if (read_bytes != file_size) {
        LOGE("WebP 读取不完整");
        free(webp_data);
        return false;
    }

    // 初始化 WebP 数据结构
    WebPData webp = {webp_data, file_size};

    // 初始化解码器选项（启用多线程加速解码）
    WebPAnimDecoderOptions options;
    WebPAnimDecoderOptionsInit(&options);
    options.color_mode = MODE_RGBA;
    options.use_threads = true;

    // 创建 WebP 动画解码器
    WebPAnimDecoder* decoder = WebPAnimDecoderNew(&webp, &options);
    if (!decoder) {
        LOGE("WebP 解码器初始化失败");
        free(webp_data);
        return false;
    }

    // 获取动画信息
    WebPAnimInfo anim_info;
    WebPAnimDecoderGetInfo(decoder, &anim_info);
    LOGD("动画信息: 宽=%d, 高=%d, 帧数=%d", anim_info.canvas_width, anim_info.canvas_height, anim_info.frame_count);

    // 初始化 GIF 写入器
    // 最后一个参数 dither = true 启用 Floyd-Steinberg 抖动，大幅减轻 256 色下的锯齿/色带
    GifWriter gif_writer;
    if (!GifBegin(&gif_writer, gif_path.c_str(), anim_info.canvas_width, anim_info.canvas_height, 1, 8, true)) {
        LOGE("GIF 创建失败: %s", gif_path.c_str());
        WebPAnimDecoderDelete(decoder);
        free(webp_data);
        return false;
    }

    // 逐帧处理
    int prev_ts = 0;
    int curr_ts = 0;
    uint8_t* frame_data = nullptr;
    bool success = true;
    int numPixels = (int)(anim_info.canvas_width * anim_info.canvas_height);

    // 创建多帧调色板采样器
    PaletteSampler paletteSampler(numPixels, anim_info.frame_count);
    bool paletteReady = false;  // shared palette built flag

    // ---- A2：确定合成背景色 ----
    // WebPAnimInfo.bgfx 是 8 位 ARGB（A 在最高字节）。优先用它作为真实画布背景。
    // 若未指定不透明画布背景（alpha<255），则 fallback 到"四角小区域众数"猜测。
    // 注意：四角猜测需要首帧像素，因此最多只执行一次（bgComputed 标志）。
    uint32_t bgcolor = anim_info.bgcolor;
    uint8_t bgA = (uint8_t)(bgcolor >> 24);
    uint8_t bgR = (uint8_t)(bgcolor >> 16);
    uint8_t bgG = (uint8_t)(bgcolor >> 8);
    uint8_t bgB = (uint8_t)(bgcolor);
    bool bgComputed = (bgA >= 255);   // 已拿到真实背景时无需再猜
    if (bgComputed) {
        LOGD("使用 WebP bgfx 背景色: R=%d G=%d B=%d", bgR, bgG, bgB);
    }

    // 帧间差异优化：缓存上一帧用于 diff 比较
    uint8_t* prevFrame = (uint8_t*)malloc((size_t)numPixels * 4);
    if (!prevFrame) {
        LOGE("分配差异检测缓冲区失败");
        GifEnd(&gif_writer);
        WebPAnimDecoderDelete(decoder);
        free(webp_data);
        return false;
    }
    bool hasPrevFrame = false;
    int accumulatedDelay = 0;

    // 用第一帧生成共享调色板，后续帧直接复用（省掉每帧的中切分，大幅提速）
    GifPalette* sharedPal = nullptr;

    while (WebPAnimDecoderHasMoreFrames(decoder)) {
        if (!WebPAnimDecoderGetNext(decoder, &frame_data, &curr_ts)) {
            LOGE("获取帧数据失败");
            success = false;
            break;
        }

        // 首帧拿到后，若 bgfx 未指定背景，则用首帧四角众数猜测（只算一次）
        if (!bgComputed) {
            guessBackgroundFromCorners(frame_data, anim_info.canvas_width, anim_info.canvas_height, bgR, bgG, bgB);
            LOGD("bgfx 未指定，fallback 猜测背景色: R=%d G=%d B=%d", bgR, bgG, bgB);
            bgComputed = true;
        }

        // A2 预处理：正确的预合成管线（按 alpha 三段处理 + 真实背景）
        compositeOntoBackground(frame_data, numPixels, bgR, bgG, bgB);

        // 计算延迟（转换为 GIF 单位：1/100 秒）
        int frameDelay = (curr_ts - prev_ts) / 10;
        if (frameDelay < 1) frameDelay = 1;

        // 第一帧：采集样本，使用局部调色板写入
        if (!hasPrevFrame) {
            if (!paletteReady) paletteSampler.addFrame(frame_data, numPixels);
            
            // 暂时不设共享调色板，用局部调色板写入
            if (!GifWriteFrame(&gif_writer, frame_data, anim_info.canvas_width, anim_info.canvas_height,
                              frameDelay, 8, true, 0, 0, 1)) {  // disposal=1 (保留给下帧做 delta)
                LOGE("写入 GIF 帧失败");
                success = false;
                break;
            }
            
            memcpy(prevFrame, frame_data, numPixels * 4);
            hasPrevFrame = true;
            prev_ts = curr_ts;
            continue;
        }

        // 后续帧：检测差异区域
        int diffLeft = 0, diffTop = 0, diffRight = 0, diffBottom = 0;
        bool hasDiff = GifComputeDiffRect(prevFrame, frame_data,
                                          anim_info.canvas_width, anim_info.canvas_height,
                                          &diffLeft, &diffTop, &diffRight, &diffBottom);

        if (!hasDiff) {
            // 无变化帧：也采集样本用于调色板
            if (!paletteReady) paletteSampler.addFrame(frame_data, numPixels);
            // 无变化：累积延迟并跳过写入（最后一帧不能跳过）
            bool isLastFrame = !WebPAnimDecoderHasMoreFrames(decoder);
            int totalDelay = frameDelay + accumulatedDelay;

            if (isLastFrame) {
                // 最后一帧即使无变化也要写出，保证延迟不丢失
                accumulatedDelay = 0;
                if (!GifWriteFrame(&gif_writer, frame_data,
                                  anim_info.canvas_width, anim_info.canvas_height,
                                  totalDelay, 8, true, 0, 0, 1)) {
                    LOGE("写入 GIF 帧失败");
                    success = false;
                    break;
                }
                memcpy(prevFrame, frame_data, numPixels * 4);
            } else {
                accumulatedDelay += frameDelay;
            }
            prev_ts = curr_ts;
            // 无变化时不更新 prevFrame — 内容相同
            continue;
        }

        // 有差异：将累积延迟合并到当前帧
        int totalDelay = frameDelay + accumulatedDelay;
        accumulatedDelay = 0;

        // 采集样本用于共享调色板
        if (!paletteReady) paletteSampler.addFrame(frame_data, numPixels);

        // 如果采样足够但调色板还未生成，现在生成共享调色板
        if (!paletteReady && paletteSampler.hasEnough()) {
            sharedPal = (GifPalette*)malloc(sizeof(GifPalette));
            paletteSampler.buildPalette(sharedPal);
            gif_writer.sharedPalette = sharedPal;
            paletteReady = true;
            LOGD("共享调色板已就绪: 采样数=%d", paletteSampler.sampleCount());
        }

        int diffW = diffRight - diffLeft;
        int diffH = diffBottom - diffTop;
        float changeRatio = (float)(diffW * diffH) / (float)(anim_info.canvas_width * anim_info.canvas_height);

        if (changeRatio < 0.3f) {
            // 小范围变化：局部帧 + disposal=1（不恢复背景，保留像素给下帧做 delta）
            if (!GifWriteFrame(&gif_writer, frame_data,
                              (uint32_t)diffW, (uint32_t)diffH,
                              totalDelay, 8, true, diffLeft, diffTop, 1)) {
                LOGE("写入 GIF 帧失败");
                success = false;
                break;
            }
        } else {
            // 大范围变化：完整帧 + disposal=1（不恢复背景，覆盖全画布等效于整体替换）
            if (!GifWriteFrame(&gif_writer, frame_data,
                              anim_info.canvas_width, anim_info.canvas_height,
                              totalDelay, 8, true, 0, 0, 1)) {
                LOGE("写入 GIF 帧失败");
                success = false;
                break;
            }
        }

        // 保存帧用于下次比较
        memcpy(prevFrame, frame_data, numPixels * 4);
        prev_ts = curr_ts;
    }

    // 如果动画帧数太少导致未生成共享调色板，现在生成
    if (!paletteReady && paletteSampler.sampleCount() > 0) {
        sharedPal = (GifPalette*)malloc(sizeof(GifPalette));
        paletteSampler.buildPalette(sharedPal);
        gif_writer.sharedPalette = sharedPal;
        paletteReady = true;
        LOGD("共享调色板(尾部生成): 采样数=%d", paletteSampler.sampleCount());
    }

    free(prevFrame);

    // 资源清理
    GifEnd(&gif_writer);
    WebPAnimDecoderDelete(decoder);
    free(webp_data);
    if(sharedPal) free(sharedPal);

    return success;
}
