#include "webp2gif.h"
#include "gif.h"
#include "webp/demux.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <android/log.h>

#define LOG_TAG "Webp2Gif"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// 提升暗部色阶，让 Floyd-Steinberg 抖动在暗区不产生明显噪点
// 对 RGB 三个通道做 gamma 校正（只提亮暗部，不影响亮部）
static void liftDarkAreas(uint8_t* frame, int numPixels) {
    // gamma < 1 提亮暗部，1.8 是经验值，对黑色背景 WebP 效果明显
    const double gamma = 1.8;
    uint8_t lut[256];
    for (int i = 0; i < 256; ++i) {
        double normalized = i / 255.0;
        double corrected = 255.0 * pow(normalized, 1.0 / gamma);
        lut[i] = (uint8_t)(corrected > 255.0 ? 255 : (corrected < 0.0 ? 0 : corrected));
    }
    for (int i = 0; i < numPixels * 4; i += 4) {
        frame[i + 0] = lut[frame[i + 0]];
        frame[i + 1] = lut[frame[i + 1]];
        frame[i + 2] = lut[frame[i + 2]];
        // alpha 通道不动
    }
}

// 把 RGBA 预混合到实色背景，消除半透明边缘导致的锯齿
static void alphaBlendOntoBackground(uint8_t* frame, int numPixels, uint8_t bgR, uint8_t bgG, uint8_t bgB) {
    for (int i = 0; i < numPixels * 4; i += 4) {
        uint8_t a = frame[i + 3];
        if (a == 255) continue;
        if (a == 0) {
            frame[i + 0] = bgR;
            frame[i + 1] = bgG;
            frame[i + 2] = bgB;
            continue;
        }
        float alpha = a / 255.0f;
        frame[i + 0] = (uint8_t)(frame[i + 0] * alpha + bgR * (1.0f - alpha) + 0.5f);
        frame[i + 1] = (uint8_t)(frame[i + 1] * alpha + bgG * (1.0f - alpha) + 0.5f);
        frame[i + 2] = (uint8_t)(frame[i + 2] * alpha + bgB * (1.0f - alpha) + 0.5f);
        frame[i + 3] = 255;
    }
}

// 采样四角，猜测背景色
static void guessBackground(uint8_t* frame, int width, int height, uint8_t& bgR, uint8_t& bgG, uint8_t& bgB) {
    auto pixelAt = [&](int x, int y) -> uint8_t* {
        return frame + (y * width + x) * 4;
    };
    int r = 0, g = 0, b = 0;
    int xs[] = {0, width - 1, 0, width - 1};
    int ys[] = {0, 0, height - 1, height - 1};
    for (int i = 0; i < 4; ++i) {
        uint8_t* p = pixelAt(xs[i], ys[i]);
        r += p[0]; g += p[1]; b += p[2];
    }
    bgR = (uint8_t)(r / 4);
    bgG = (uint8_t)(g / 4);
    bgB = (uint8_t)(b / 4);
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

    // 初始化解码器选项（禁用线程）
    WebPAnimDecoderOptions options;
    WebPAnimDecoderOptionsInit(&options);
    options.color_mode = MODE_RGBA;
    options.use_threads = false;

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

    // 用第一帧猜背景色（同一动图背景通常不变）
    uint8_t bgR = 0, bgG = 0, bgB = 0;
    bool bgGuessed = false;

    while (WebPAnimDecoderHasMoreFrames(decoder)) {
        if (!WebPAnimDecoderGetNext(decoder, &frame_data, &curr_ts)) {
            LOGE("获取帧数据失败");
            success = false;
            break;
        }

        // 预处理 1：RGBA → 预混合到实色背景，消除半透明边缘锯齿
        if (!bgGuessed) {
            guessBackground(frame_data, anim_info.canvas_width, anim_info.canvas_height, bgR, bgG, bgB);
            LOGD("猜测背景色: R=%d G=%d B=%d", bgR, bgG, bgB);
            bgGuessed = true;
        }
        alphaBlendOntoBackground(frame_data, numPixels, bgR, bgG, bgB);

        // 预处理 2：提亮暗部，减轻抖动在暗区的颗粒感
        liftDarkAreas(frame_data, numPixels);

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

    return success;
}