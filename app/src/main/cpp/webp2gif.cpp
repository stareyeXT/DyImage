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
// 2) 按 alpha 分三段：>250 保留前景、<6 直接填背景（去掉半幽灵边缘）、中间用正确 over 公式合成
// 3) 抖动前把被污染的半透明边缘清理干净
static void compositeOntoBackground(uint8_t* frame, int numPixels, uint8_t bgR, uint8_t bgG, uint8_t bgB) {
    const int ALPHA_FLOOR = 6;      // 低于此 alpha 视为背景（去掉半幽灵边缘）
    const int ALPHA_CEIL  = 250;    // 高于此 alpha 视为不透明前景（保留）
    for (int i = 0; i < numPixels * 4; i += 4) {
        uint8_t a = frame[i + 3];
        if (a >= ALPHA_CEIL) continue;            // 前景保留不动
        if (a <= ALPHA_FLOOR) {                   // 填背景，去掉半幽灵像素
            frame[i + 0] = bgR;
            frame[i + 1] = bgG;
            frame[i + 2] = bgB;
            frame[i + 3] = 255;
            continue;
        }
        // 中间 alpha：正确 over 合成
        float alpha = a / 255.0f;
        frame[i + 0] = (uint8_t)(frame[i + 0] * alpha + bgR * (1.0f - alpha) + 0.5f);
        frame[i + 1] = (uint8_t)(frame[i + 1] * alpha + bgG * (1.0f - alpha) + 0.5f);
        frame[i + 2] = (uint8_t)(frame[i + 2] * alpha + bgB * (1.0f - alpha) + 0.5f);
        frame[i + 3] = 255;
    }
}

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

        // 第一帧：生成共享调色板
        if(!sharedPal) {
            sharedPal = (GifPalette*)malloc(sizeof(GifPalette));
            GifMakePalette(NULL, frame_data, anim_info.canvas_width, anim_info.canvas_height, 8, true, sharedPal);
            gif_writer.sharedPalette = sharedPal;
            LOGD("共享调色板已生成");
        }

        // 计算延迟（转换为 GIF 单位：1/100 秒）
        int delay = (curr_ts - prev_ts) / 10;
        if (delay < 1) delay = 1; // 最小延迟 10ms

        // 写入 GIF 帧，启用抖动
        if (!GifWriteFrame(&gif_writer, frame_data, anim_info.canvas_width, anim_info.canvas_height, delay, 8, true)) {
            LOGE("写入 GIF 帧失败");
            success = false;
            break;
        }

        prev_ts = curr_ts;
    }

    // 资源清理
    GifEnd(&gif_writer);
    WebPAnimDecoderDelete(decoder);
    free(webp_data);
    if(sharedPal) free(sharedPal);

    return success;
}
