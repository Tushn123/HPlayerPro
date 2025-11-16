#include "hffplayer.h"

#include "confile.h"
#include "hlog.h"
#include "hstring.h"
#include "hscope.h"
#include "htime.h"
extern "C"{
#include <libavutil/time.h>
}


#include <cstring>
#include <cmath>

#define DEFAULT_BLOCK_TIMEOUT   10  // 秒

std::atomic_flag HFFPlayer::s_ffmpeg_init = ATOMIC_FLAG_INIT;

// 列出设备
static void list_devices() {
    AVFormatContext* fmt_ctx = avformat_alloc_context();
    AVDictionary* options = NULL;
    av_dict_set(&options, "list_devices", "true", 0);
#ifdef _WIN32
    const char drive[] = "dshow";
#elif defined(__linux__)
    const char drive[] = "v4l2";
#else
    const char drive[] = "avfoundation";
#endif
    const AVInputFormat* ifmt = av_find_input_format(drive);
    if (ifmt) {
        avformat_open_input(&fmt_ctx, "video=dummy", (AVInputFormat*)ifmt, &options);
    }
    avformat_close_input(&fmt_ctx);
    avformat_free_context(fmt_ctx);
    av_dict_free(&options);
}

// 调试所有硬件解码器
static void debug_all_hardware_decoders() {
    const AVCodec* codec = NULL;
    void* iter = NULL;

    hlogi("=== 可用的硬件解码器 ===");
    hlogi("FFmpeg 版本: %s", av_version_info());

    // 列出所有硬件设备类型
    hlogi("\n--- 支持的硬件设备类型 ---");
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
        hlogi("  硬件设备: %s", av_hwdevice_get_type_name(type));
    }

    // 列出所有硬件解码器
    hlogi("\n--- 可用的硬件解码器 ---");
    int hw_decoder_count = 0;
    while ((codec = av_codec_iterate(&iter))) {
        if (av_codec_is_decoder(codec)) {
            // 通过名称后缀检查是否是硬件解码器
            const char* name = codec->name;
            bool is_hw = false;

            if (strstr(name, "_cuvid") || strstr(name, "_qsv") ||
                strstr(name, "_dxva2") || strstr(name, "_d3d11va") ||
                strstr(name, "_videotoolbox") || strstr(name, "_vaapi") ||
                strstr(name, "_vdpau") || strstr(name, "_mediacodec")) {
                is_hw = true;
            }

            // 同时检查硬件能力标志
            if (codec->capabilities & AV_CODEC_CAP_HARDWARE) {
                is_hw = true;
            }

            if (is_hw) {
                hw_decoder_count++;
                hlogi("  [%d] 硬件解码器: %-25s (%s)",
                      hw_decoder_count, codec->name, codec->long_name);

                // 检查支持的硬件配置
                for (int i = 0; ; i++) {
                    const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
                    if (!config) break;

                    const char* hw_type = av_hwdevice_get_type_name(config->device_type);
                    if (hw_type) {
                        hlogi("      -> 支持设备: %s (方法: %d)",
                              hw_type, config->methods);
                    }
                }
            }
        }
    }

    if (hw_decoder_count == 0) {
        hlogi("  未找到硬件解码器!");
        hlogi("  注意: 您可能需要重新编译FFmpeg以支持硬件加速");
    } else {
        hlogi("\n找到的硬件解码器总数: %d", hw_decoder_count);
    }

    hlogi("=== 硬件解码器列表结束 ===\n");
}

// 注意: avformat_open_input,av_read_frame 会阻塞
static int interrupt_callback(void* opaque) {
    if (opaque == NULL) return 0;
    HFFPlayer* player = (HFFPlayer*)opaque;
    if (player->quit ||
        time(NULL) - player->block_starttime > player->block_timeout) {
        hlogi("中断退出 quit=%d media.src=%s", player->quit, player->media.src.c_str());
        return 1;
    }
    return 0;
}

HFFPlayer::HFFPlayer()
    : HVideoPlayer()
    , HThread() {
    fmt_opts = NULL;
    codec_opts = NULL;
    fmt_ctx = NULL;
    codec_ctx = NULL;
    audio_codec_ctx = NULL;
    packet = NULL;
    frame = NULL;
    audio_frame = NULL;
    sws_ctx = NULL;
    sws_ctx_checked = false;
    swr_ctx = NULL;
    audio_buffer = NULL;
    audio_buffer_size = 0;
    audio_channels = 0;
    audio_sample_rate = 0;

    // 初始化SDL音频
    audio_dev_id = 0;
    audio_hw_buf_size = 0;
    audio_play_buf = NULL;
    audio_play_buf_size = 0;
    audio_play_buf_index = 0;
    hmutex_init(&audio_queue_mutex);

    // 初始化时钟
    init_clock(&audio_clock);
    init_clock(&video_clock);

    // 从配置文件读取av_sync_type
    // 0=音频主时钟(默认), 1=视频主时钟, 2=外部时钟
    std::string sync_type_str = g_confile->GetValue("av_sync_type", "video");
    if (sync_type_str == "video") {
        av_sync_type = 1;  // 视频主时钟
    } else if (sync_type_str == "external") {
        av_sync_type = 2;  // 外部时钟
    } else {
        av_sync_type = 0;  // 音频主时钟(默认)
    }
    hlogi("AV同步类型: %d (%s)", av_sync_type,
          av_sync_type == 0 ? "音频主时钟" : av_sync_type == 1 ? "视频主时钟" : "外部时钟");

    audio_diff_cum = 0;
    audio_diff_avg_coef = exp(log(0.01) / 20);
    audio_diff_avg_count = 0;
    frame_timer = 0;
    frame_last_pts = 0;
    frame_last_delay = 0;

    block_starttime = time(NULL);
    block_timeout = DEFAULT_BLOCK_TIMEOUT;
    quit = 0;

    // 初始化线程同步
    hmutex_init(&decoder_mutex);
    hmutex_init(&audio_queue_mutex);
    hmutex_init(&format_mutex);
    is_seeking.store(false);


    if (!s_ffmpeg_init.test_and_set()) {
        // av_register_all();
        // avcodec_register_all();
        avformat_network_init();
        avdevice_register_all();
        list_devices();

        // 调试: 列出所有可用的硬件解码器
        debug_all_hardware_decoders();
    }
}

HFFPlayer::~HFFPlayer() {
    close();
    hmutex_destroy(&decoder_mutex);
    hmutex_destroy(&audio_queue_mutex);
    hmutex_destroy(&format_mutex);
}

int HFFPlayer::open() {
    std::string ifile;

    const AVInputFormat* ifmt = NULL;
    switch (media.type) {
    case MEDIA_TYPE_CAPTURE:
    {
        ifile = "video=";
        ifile += media.src;
#ifdef _WIN32
        const char drive[] = "dshow";
#elif defined(__linux__)
        const char drive[] = "v4l2";
#else
        const char drive[] = "avfoundation";
#endif
        ifmt = av_find_input_format(drive);
        if (ifmt == NULL) {
            hloge("找不到 dshow");
            return -5;
        }
    }
    break;
    case MEDIA_TYPE_FILE:
    case MEDIA_TYPE_NETWORK:
        ifile = media.src;
        break;
    default:
        return -10;
    }

    hlogi("输入文件:%s", ifile.c_str());
    int ret = 0;
    fmt_ctx = avformat_alloc_context();
    if (fmt_ctx == NULL) {
        hloge("avformat_alloc_context");
        ret = -10;
        return ret;
    }
    defer (if (ret != 0 && fmt_ctx) {avformat_free_context(fmt_ctx); fmt_ctx = NULL;})

        if (media.type == MEDIA_TYPE_NETWORK) {
        if (strncmp(media.src.c_str(), "rtsp:", 5) == 0) {
            std::string str = g_confile->GetValue("rtsp_transport", "video");
            if (strcmp(str.c_str(), "tcp") == 0 ||
                strcmp(str.c_str(), "udp") == 0) {
                av_dict_set(&fmt_opts, "rtsp_transport", str.c_str(), 0);
            }
        }
        av_dict_set(&fmt_opts, "stimeout", "5000000", 0);   // 微秒
    }
    av_dict_set(&fmt_opts, "buffer_size", "2048000", 0);
    fmt_ctx->interrupt_callback.callback = interrupt_callback;
    fmt_ctx->interrupt_callback.opaque = this;
    block_starttime = time(NULL);
    ret = avformat_open_input(&fmt_ctx, ifile.c_str(), (AVInputFormat*)ifmt, &fmt_opts);
    if (ret != 0) {
        hloge("打开输入文件[%s]失败: %d", ifile.c_str(), ret);
        return ret;
    }
    fmt_ctx->interrupt_callback.callback = NULL;
    defer (if (ret != 0 && fmt_ctx) {avformat_close_input(&fmt_ctx);})

        ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret != 0) {
        hloge("找不到流信息: %d", ret);
        return ret;
    }
    hlogi("流数量=%d", fmt_ctx->nb_streams);

    video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    audio_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    subtitle_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
    hlogi("视频流索引=%d", video_stream_index);
    hlogi("音频流索引=%d", audio_stream_index);
    hlogi("字幕流索引=%d", subtitle_stream_index);

    if (video_stream_index < 0) {
        hloge("找不到视频流.");
        ret = -20;
        return ret;
    }

    AVStream* video_stream = fmt_ctx->streams[video_stream_index];
    video_time_base_num = video_stream->time_base.num;
    video_time_base_den = video_stream->time_base.den;
    hlogi("视频流 time_base=%d/%d", video_stream->time_base.num, video_stream->time_base.den);

    AVCodecParameters* codec_param = video_stream->codecpar;
    hlogi("编解码器ID=%d:%s", codec_param->codec_id, avcodec_get_name(codec_param->codec_id));

    const AVCodec* codec = NULL;
    if (decode_mode != SOFTWARE_DECODE) {
    try_hardware_decode:
        std::string codec_name(avcodec_get_name(codec_param->codec_id));
        std::string decoder_name;
        bool found = false;

        if (decode_mode == HARDWARE_DECODE_AUTO) {
            // 自动模式: 按优先级尝试所有硬件解码器
            // Windows优先级: CUVID(NVIDIA) > QSV(Intel) > D3D11VA > DXVA2
            const char* hw_suffixes[] = {"_cuvid", "_qsv", "_d3d11va", "_dxva2"};
            const char* hw_names[] = {"NVIDIA CUVID", "Intel QSV", "D3D11VA", "DXVA2"};
            const int hw_modes[] = {HARDWARE_DECODE_CUVID, HARDWARE_DECODE_QSV,
                                    HARDWARE_DECODE_D3D11VA, HARDWARE_DECODE_DXVA2};

            for (int i = 0; i < 4; ++i) {
                decoder_name = codec_name + hw_suffixes[i];
                codec = avcodec_find_decoder_by_name(decoder_name.c_str());

                if (codec != NULL) {
                    real_decode_mode = hw_modes[i];
                    hlogi("找到硬件解码器: %s (%s)", decoder_name.c_str(), hw_names[i]);
                    found = true;
                    break;
                } else {
                    hlogi("硬件解码器不可用: %s (%s)", decoder_name.c_str(), hw_names[i]);
                }
            }
        } else {
            // 手动模式: 尝试特定解码器
            const char* suffix = NULL;
            const char* hw_name = NULL;

            if (decode_mode == HARDWARE_DECODE_CUVID) {
                suffix = "_cuvid";
                hw_name = "NVIDIA CUVID";
            } else if (decode_mode == HARDWARE_DECODE_QSV) {
                suffix = "_qsv";
                hw_name = "Intel QSV";
            } else if (decode_mode == HARDWARE_DECODE_D3D11VA) {
                suffix = "_d3d11va";
                hw_name = "D3D11VA";
            } else if (decode_mode == HARDWARE_DECODE_DXVA2) {
                suffix = "_dxva2";
                hw_name = "DXVA2";
            }

            if (suffix != NULL) {
                decoder_name = codec_name + suffix;
                codec = avcodec_find_decoder_by_name(decoder_name.c_str());

                if (codec != NULL) {
                    real_decode_mode = decode_mode;
                    hlogi("找到硬件解码器: %s (%s)", decoder_name.c_str(), hw_name);
                    found = true;
                } else {
                    hlogi("硬件解码器不可用: %s (%s)", decoder_name.c_str(), hw_name);
                }
            }
        }

        if (!found) {
            hlogi("未找到硬件解码器，将尝试软件解码");
        }
    }

    if (codec == NULL) {
    try_software_decode:
        codec = avcodec_find_decoder(codec_param->codec_id);
        if (codec == NULL) {
            hloge("找不到解码器 %s", avcodec_get_name(codec_param->codec_id));
            ret = -30;
            return ret;
        }
        real_decode_mode = SOFTWARE_DECODE;
        hlogi("使用软件解码器: %s", codec->name);
    }

    hlogi("编解码器名称: %s=>%s", codec->name, codec->long_name);

    codec_ctx = avcodec_alloc_context3(codec);
    if (codec_ctx == NULL) {
        hloge("avcodec_alloc_context3");
        ret = -40;
        return ret;
    }
    defer (if (ret != 0 && codec_ctx) {avcodec_free_context(&codec_ctx); codec_ctx = NULL;})

        ret = avcodec_parameters_to_context(codec_ctx, codec_param);
    if (ret != 0) {
        hloge("avcodec_parameters_to_context 错误: %d", ret);
        return ret;
    }

    if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        av_dict_set(&codec_opts, "refcounted_frames", "1", 0);
    }
    ret = avcodec_open2(codec_ctx, codec, &codec_opts);
    if (ret != 0) {
        if (real_decode_mode != SOFTWARE_DECODE) {
            hlogi("无法打开硬件编解码器 错误: %d, 尝试软件编解码器.", ret);
            // 清理失败的硬件解码器上下文
            avcodec_free_context(&codec_ctx);
            codec_ctx = NULL;
            // 查找软件解码器
            codec = avcodec_find_decoder(codec_param->codec_id);
            if (codec == NULL) {
                hloge("找不到软件解码器 %s", avcodec_get_name(codec_param->codec_id));
                ret = -30;
                return ret;
            }
            real_decode_mode = SOFTWARE_DECODE;
            hlogi("使用软件解码器: %s", codec->name);

            // 重新分配解码器上下文
            codec_ctx = avcodec_alloc_context3(codec);
            if (codec_ctx == NULL) {
                hloge("avcodec_alloc_context3");
                ret = -40;
                return ret;
            }

            ret = avcodec_parameters_to_context(codec_ctx, codec_param);
            if (ret != 0) {
                hloge("avcodec_parameters_to_context 错误: %d", ret);
                return ret;
            }

            ret = avcodec_open2(codec_ctx, codec, &codec_opts);
            if (ret != 0) {
                hloge("无法打开软件编解码器 错误: %d", ret);
                return ret;
            }
        } else {
            hloge("无法打开软件编解码器 错误: %d", ret);
            return ret;
        }
    }
    video_stream->discard = AVDISCARD_DEFAULT;

    int sw, sh, dw, dh;
    sw = codec_ctx->width;
    sh = codec_ctx->height;
    src_pix_fmt = codec_ctx->pix_fmt;
    hlogi("源宽度=%d 源高度=%d 源像素格式=%d:%s", sw, sh, src_pix_fmt, av_get_pix_fmt_name(src_pix_fmt));
    if (sw <= 0 || sh <= 0 || src_pix_fmt == AV_PIX_FMT_NONE) {
        hloge("编解码器参数错误!");
        ret = -45;
        return ret;
    }

    dw = sw >> 2 << 2; // 对齐 = 4
    dh = sh;
    dst_pix_fmt = AV_PIX_FMT_YUV420P;
    std::string str = g_confile->GetValue("dst_pix_fmt", "video");
    if (!str.empty()) {
        if (strcmp(str.c_str(), "YUV") == 0) {
            dst_pix_fmt = AV_PIX_FMT_YUV420P;
        }
        else if (strcmp(str.c_str(), "RGB") == 0) {
            dst_pix_fmt = AV_PIX_FMT_BGR24;
        }
    }
    hlogi("目标宽度=%d 目标高度=%d 目标像素格式=%d:%s", dw, dh, dst_pix_fmt, av_get_pix_fmt_name(dst_pix_fmt));

    sws_ctx = sws_getContext(sw, sh, src_pix_fmt, dw, dh, dst_pix_fmt, SWS_BICUBIC, NULL, NULL, NULL);
    if (sws_ctx == NULL) {
        hloge("sws_getContext");
        ret = -50;
        return ret;
    }

    // 重置标志，在第一个解码帧上检查格式
    sws_ctx_checked = false;

    packet = av_packet_alloc();
    frame = av_frame_alloc();

    hframe.w = dw;
    hframe.h = dh;
    // ARGB
    hframe.buf.resize(dw * dh * 4);

    if (dst_pix_fmt == AV_PIX_FMT_YUV420P) {
        hframe.type = PIX_FMT_IYUV;
        hframe.bpp = 12;
        int y_size = dw * dh;
        hframe.buf.len = y_size * 3 / 2;
        data[0] = (uint8_t*)hframe.buf.base;
        data[1] = data[0] + y_size;
        data[2] = data[1] + y_size / 4;
        linesize[0] = dw;
        linesize[1] = linesize[2] = dw / 2;
    }
    else {
        dst_pix_fmt = AV_PIX_FMT_BGR24;
        hframe.type = PIX_FMT_BGR;
        hframe.bpp = 24;
        hframe.buf.len = dw * dh * 3;
        data[0] = (uint8_t*)hframe.buf.base;
        linesize[0] = dw * 3;
    }

    // HVideoPlayer 成员变量
    if (video_stream->avg_frame_rate.num && video_stream->avg_frame_rate.den) {
        fps = video_stream->avg_frame_rate.num / video_stream->avg_frame_rate.den;
    }
    width = sw;
    height = sh;
    duration = 0;
    start_time = 0;
    eof = 0;
    error = 0;
    if (video_time_base_num && video_time_base_den) {
        if (video_stream->duration > 0) {
            duration = video_stream->duration / (double)video_time_base_den * video_time_base_num * 1000;
        }
        if (video_stream->start_time > 0) {
            start_time = video_stream->start_time / (double)video_time_base_den * video_time_base_num * 1000;
        }
    }
    hlogi("帧率=%d 时长=%lld毫秒 开始时间=%lld毫秒", fps, duration, start_time);

    // 如果存在音频流，初始化音频解码器
    if (audio_stream_index >= 0) {
        AVStream* audio_stream = fmt_ctx->streams[audio_stream_index];
        audio_time_base_num = audio_stream->time_base.num;
        audio_time_base_den = audio_stream->time_base.den;
        hlogi("音频流 time_base=%d/%d", audio_stream->time_base.num, audio_stream->time_base.den);

        AVCodecParameters* audio_codec_param = audio_stream->codecpar;
        hlogi("音频编解码器ID=%d:%s", audio_codec_param->codec_id, avcodec_get_name(audio_codec_param->codec_id));

        const AVCodec* audio_codec = avcodec_find_decoder(audio_codec_param->codec_id);
        if (audio_codec == NULL) {
            hloge("找不到音频解码器 %s", avcodec_get_name(audio_codec_param->codec_id));
            // 音频是可选的，继续但没有音频
        } else {
            hlogi("音频编解码器名称: %s=>%s", audio_codec->name, audio_codec->long_name);

            audio_codec_ctx = avcodec_alloc_context3(audio_codec);
            if (audio_codec_ctx == NULL) {
                hloge("音频解码器 avcodec_alloc_context3 失败");
            } else {
                ret = avcodec_parameters_to_context(audio_codec_ctx, audio_codec_param);
                if (ret != 0) {
                    hloge("音频解码器 avcodec_parameters_to_context 错误: %d", ret);
                    avcodec_free_context(&audio_codec_ctx);
                    audio_codec_ctx = NULL;
                } else {
                    ret = avcodec_open2(audio_codec_ctx, audio_codec, NULL);
                    if (ret != 0) {
                        hloge("无法打开音频编解码器 错误: %d", ret);
                        avcodec_free_context(&audio_codec_ctx);
                        audio_codec_ctx = NULL;
                    } else {
                        audio_stream->discard = AVDISCARD_DEFAULT;
                        audio_frame = av_frame_alloc();

                        // 为PCM S16LE立体声输出初始化音频重采样器
                        audio_channels = 2;
                        audio_sample_rate = 44100;

                        // 如果可用，使用新的声道布局API
                        int64_t in_ch_layout = AV_CH_LAYOUT_STEREO;
                        if (audio_codec_ctx->channel_layout) {
                            in_ch_layout = audio_codec_ctx->channel_layout;
                        } else if (audio_codec_ctx->channels > 0) {
                            in_ch_layout = av_get_default_channel_layout(audio_codec_ctx->channels);
                        }

                        swr_ctx = swr_alloc_set_opts(NULL,
                                                     AV_CH_LAYOUT_STEREO,          // 输出声道布局
                                                     AV_SAMPLE_FMT_S16,            // 输出采样格式
                                                     audio_sample_rate,            // 输出采样率
                                                     in_ch_layout,                 // 输入声道布局
                                                     audio_codec_ctx->sample_fmt,  // 输入采样格式
                                                     audio_codec_ctx->sample_rate, // 输入采样率
                                                     0, NULL);

                        if (swr_ctx) {
                            ret = swr_init(swr_ctx);
                            if (ret < 0) {
                                hloge("swr_init 失败: %d", ret);
                                swr_free(&swr_ctx);
                                swr_ctx = NULL;
                            } else {
                                // 分配音频输出缓冲区
                                audio_buffer_size = av_samples_get_buffer_size(NULL, audio_channels,
                                                                               audio_codec_ctx->frame_size > 0 ? audio_codec_ctx->frame_size : 1024,
                                                                               AV_SAMPLE_FMT_S16, 1);
                                audio_buffer = (uint8_t*)av_malloc(audio_buffer_size);

                                hlogi("音频解码器已初始化: 声道数=%d, 采样率=%d",
                                      audio_channels, audio_sample_rate);

                                // 打开SDL音频设备
                                if (audio_open() < 0) {
                                    hloge("打开SDL音频设备失败");
                                    // 继续但没有音频
                                } else {
                                    hlogi("SDL音频设备成功打开");
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // 如果没有音频，使用视频作为主时钟
    if (audio_stream_index < 0 || !audio_codec_ctx || audio_dev_id == 0) {
        hlogi("没有音频流或音频初始化失败，使用视频时钟作为主时钟");
    }

    HThread::setSleepPolicy(HThread::SLEEP_UNTIL, 1000 / fps);
    return ret;
}

int HFFPlayer::close() {
    // 首先关闭SDL音频
    audio_close();

    if (fmt_opts) {
        av_dict_free(&fmt_opts);
        fmt_opts = NULL;
    }

    if (codec_opts) {
        av_dict_free(&codec_opts);
        codec_opts = NULL;
    }

    if (codec_ctx) {
        avcodec_close(codec_ctx);
        avcodec_free_context(&codec_ctx);
        codec_ctx = NULL;
    }

    if (fmt_ctx) {
        avformat_close_input(&fmt_ctx);
        avformat_free_context(fmt_ctx);
        fmt_ctx = NULL;
    }

    if (frame) {
        av_frame_unref(frame);
        av_frame_free(&frame);
        frame = NULL;
    }

    if (packet) {
        av_packet_unref(packet);
        av_packet_free(&packet);
        packet = NULL;
    }

    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = NULL;
    }

    if (audio_codec_ctx) {
        avcodec_close(audio_codec_ctx);
        avcodec_free_context(&audio_codec_ctx);
        audio_codec_ctx = NULL;
    }

    if (audio_frame) {
        av_frame_unref(audio_frame);
        av_frame_free(&audio_frame);
        audio_frame = NULL;
    }

    if (swr_ctx) {
        swr_free(&swr_ctx);
        swr_ctx = NULL;
    }

    if (audio_buffer) {
        av_free(audio_buffer);
        audio_buffer = NULL;
        audio_buffer_size = 0;
    }

    hframe.buf.cleanup();
    return 0;
}

void HFFPlayer::flushDecoders() {
    // 注意: 此函数应在 decoder_mutex 锁定时调用
    // 刷新视频解码器
    if (codec_ctx) {
        avcodec_flush_buffers(codec_ctx);
        hlogi("视频解码器已刷新");
    }

    // 刷新音频解码器
    if (audio_codec_ctx) {
        avcodec_flush_buffers(audio_codec_ctx);
        hlogi("音频解码器已刷新");
    }

    // 重置时钟
    init_clock(&audio_clock);
    init_clock(&video_clock);
}

int HFFPlayer::seek(int64_t ms) {
    // 默认: 快速定位(到关键帧)
    return seek(ms, false);
}

int HFFPlayer::seek(int64_t ms, bool accurate) {
    if (!fmt_ctx) {
        hloge("定位失败: fmt_ctx 为 NULL");
        return -1;
    }

    const char* seek_mode = accurate ? "accurate" : "fast";
    hlogi("定位到=>%lld毫秒 (模式=%s, 时长=%lld毫秒, 开始时间=%lld毫秒)",
          ms, seek_mode, duration, start_time);

    // 检查定位位置是否有效
    if (ms < 0) {
        hlogw("定位位置为负数，钳制到0");
        ms = 0;
    }

    if (duration > 0 && ms > duration) {
        hlogw("定位位置超过时长，钳制到时长");
        ms = duration;
    }

    // 设置定位标志，防止工作线程中的解码器操作
    is_seeking.store(true);

    // 定位前清除帧缓存
    clear_frame_cache();

    // 清除音频队列
    hmutex_lock(&audio_queue_mutex);
    while (!audio_frame_queue.empty()) {
        AudioFrame* af = audio_frame_queue.front();
        audio_frame_queue.pop();
        delete af;
    }
    hmutex_unlock(&audio_queue_mutex);

    // 重置播放缓冲区
    audio_play_buf_index = 0;
    audio_play_buf_size = 0;

    // 重置EOF标志
    eof = 0;
    error = 0;

    int ret = 0;
    int64_t seek_target = 0;
    int seek_flags = AVSEEK_FLAG_BACKWARD;

    // 对于精确定位，我们仍然定位到关键帧但解码帧直到目标
    // 对于快速定位，我们只定位到最近的关键帧
    if (accurate) {
        // 在精确模式下，我们可能希望在目标之前稍微定位
        // 以确保我们可以解码到精确的帧
        hlogi("精确定位模式: 将解码到精确位置");
    }

    // 锁定 format_mutex 以防止与 doTask() 的并发访问
    hmutex_lock(&format_mutex);

    // 尝试使用视频流时间戳定位(更准确)
    if (video_stream_index >= 0 && video_time_base_num && video_time_base_den) {
        // 以流时间基计算目标时间戳
        seek_target = (start_time + ms) / 1000.0 / video_time_base_num * video_time_base_den;

        hlogi("定位视频流: target_ms=%lld, start_time=%lld, timestamp=%lld",
              ms, start_time, seek_target);

        ret = av_seek_frame(fmt_ctx, video_stream_index, seek_target, seek_flags);

        if (ret < 0) {
            hloge("视频流 av_seek_frame 失败: %d", ret);
            // 尝试不使用流索引定位(让FFmpeg选择最佳流)
            seek_target = (start_time + ms) * 1000; // 以微秒为单位 (AV_TIME_BASE)
            ret = av_seek_frame(fmt_ctx, -1, seek_target, seek_flags);

            if (ret < 0) {
                hloge("任何流 av_seek_frame 失败: %d", ret);
                hmutex_unlock(&format_mutex);  // 返回前解锁
                is_seeking.store(false);  // 返回前清除标志
                return ret;
            } else {
                hlogi("使用默认流选择定位成功");
            }
        } else {
            hlogi("视频流定位成功");
        }
    } else {
        // 没有视频流或时间基未设置，使用默认时间戳
        seek_target = (start_time + ms) * 1000; // 以微秒为单位 (AV_TIME_BASE)
        hlogi("使用默认时间基准位: target_ms=%lld, timestamp=%lld", ms, seek_target);

        ret = av_seek_frame(fmt_ctx, -1, seek_target, seek_flags);

        if (ret < 0) {
            hloge("av_seek_frame 失败: %d", ret);
            hmutex_unlock(&format_mutex);  // 返回前解锁
            is_seeking.store(false);  // 返回前清除标志
            return ret;
        }
    }

    hmutex_unlock(&format_mutex);

    // 刷新前锁定解码器互斥锁
    hmutex_lock(&decoder_mutex);

    // 刷新解码器以清除缓冲的帧
    flushDecoders();

    hmutex_unlock(&decoder_mutex);

    // 如果存在音频流且具有不同的时间基，可选地定位音频流
    if (audio_stream_index >= 0 && audio_time_base_num && audio_time_base_den) {
        // 音频流将在播放期间自动同步
        // 但我们可以记录预期的音频时间戳
        int64_t audio_seek_target = (start_time + ms) / 1000.0 / audio_time_base_num * audio_time_base_den;
        hlogi("音频流预期时间戳: %lld", audio_seek_target);
    }

    // 清除定位标志
    is_seeking.store(false);

    hlogi("定位到 %lldms 成功 (模式=%s)", ms, seek_mode);
    return 0;
}

int HFFPlayer::seekByPercent(double percent) {
    if (duration <= 0) {
        hloge("seekByPercent 失败: 时长不可用");
        return -1;
    }

    // 将百分比钳制到有效范围
    if (percent < 0.0) {
        percent = 0.0;
    } else if (percent > 100.0) {
        percent = 100.0;
    }

    // 以毫秒计算目标位置
    int64_t target_ms = (int64_t)(duration * percent / 100.0);

    hlogi("按百分比定位: %.2f%% => %lld毫秒 (时长=%lld毫秒)", percent, target_ms, duration);

    return seek(target_ms);
}

int HFFPlayer::seekRelative(int64_t offset_ms) {
    int64_t current_pos = getCurrentPosition();

    if (current_pos < 0) {
        hloge("相对定位失败: 无法确定当前位置");
        return -1;
    }

    int64_t target_ms = current_pos + offset_ms;

    hlogi("相对定位: 当前=%lld毫秒, 偏移=%lld毫秒 => 目标=%lld毫秒",
          current_pos, offset_ms, target_ms);

    return seek(target_ms);
}

int64_t HFFPlayer::getCurrentPosition() {
    // 使用最后解码帧的时间戳
    if (hframe.ts >= 0) {
        // hframe.ts 已经是毫秒
        return hframe.ts;
    }

    // 如果还没有解码任何帧，返回0
    return 0;
}

void HFFPlayer::set_speed(double speed) {
    // 调用基类设置播放速度
    HVideoPlayer::set_speed(speed);

    // 关键: 为 HFFPlayer 架构调整解码线程睡眠时间
    // 与 ffplay 在显示循环中使用 Clock 不同，HFFPlayer 通过
    // 线程睡眠策略控制帧率
    if (fps > 0 && speed > 0.0) {
        int sleep_ms = (int)((1000.0 / fps) / speed);
        if (sleep_ms < 1) sleep_ms = 1;  // 最小1毫秒
        HThread::setSleepPolicy(HThread::SLEEP_UNTIL, sleep_ms);
        hlogi("播放速度设置为 %.2fx (解码睡眠: %d毫秒)", speed, sleep_ms);
    } else {
        hlogi("播放速度设置为 %.2fx (无睡眠调整)", speed);
    }
}

bool HFFPlayer::doPrepare() {
    int ret = open();
    if (ret != 0) {
        if (!quit) {
            error = ret;
            event_callback(HPLAYER_OPEN_FAILED);
        }
        return false;
    }
    else {
        event_callback(HPLAYER_OPENED);
    }
    return true;
}

bool HFFPlayer::doFinish() {
    int ret = close();
    event_callback(HPLAYER_CLOSED);
    return ret == 0;
}

int HFFPlayer::processVideoPacket() {
    // 如果正在定位，跳过处理
    if (is_seeking.load()) {
        return AVERROR(EAGAIN);
    }

    // 锁定解码器互斥锁以防止与定位操作的竞争条件
    hmutex_lock(&decoder_mutex);

    // 获取锁后再次检查定位标志
    if (is_seeking.load()) {
        hmutex_unlock(&decoder_mutex);
        return AVERROR(EAGAIN);
    }

    int ret = avcodec_send_packet(codec_ctx, packet);
    if (ret != 0) {
        hmutex_unlock(&decoder_mutex);
        hloge("avcodec_send_packet 错误: %d", ret);
        return ret;
    }

    ret = avcodec_receive_frame(codec_ctx, frame);
    if (ret != 0) {
        hmutex_unlock(&decoder_mutex);
        if (ret == AVERROR(EAGAIN)) {
            return ret;  // 需要更多数据包
        }
        hloge("avcodec_receive_frame 错误: %d", ret);
        return ret;
    }

    hmutex_unlock(&decoder_mutex);

    // 在第一个解码帧上，验证/重新创建具有实际帧格式的 sws_ctx
    if (!sws_ctx_checked) {
        sws_ctx_checked = true;

        AVPixelFormat actual_fmt = (AVPixelFormat)frame->format;

        // 检查帧格式是否与 sws_ctx 期望的匹配
        if (actual_fmt != src_pix_fmt || frame->width != width || frame->height != height) {
            hlogi("检测到帧格式不匹配! 重新创建 sws_ctx...");
            hlogi("预期: %dx%d 格式=%s(%d)", width, height,
                  av_get_pix_fmt_name(src_pix_fmt), src_pix_fmt);
            hlogi("实际: %dx%d 格式=%s(%d)", frame->width, frame->height,
                  av_get_pix_fmt_name(actual_fmt), actual_fmt);

            // 释放旧的 sws_ctx
            if (sws_ctx) {
                sws_freeContext(sws_ctx);
                sws_ctx = NULL;
            }

            // 更新存储的值
            width = frame->width;
            height = frame->height;
            src_pix_fmt = actual_fmt;

            // 使用实际帧参数重新创建
            int dw = frame->width >> 2 << 2;  // 对齐到4
            int dh = frame->height;

            sws_ctx = sws_getContext(
                frame->width, frame->height, actual_fmt,  // 实际源
                dw, dh, dst_pix_fmt,                      // 目标
                SWS_BICUBIC, NULL, NULL, NULL);

            if (!sws_ctx) {
                hloge("重新创建 sws_ctx 失败!");
                return -1;
            }

            // 更新 hframe 尺寸
            hframe.w = dw;
            hframe.h = dh;
            hframe.buf.resize(dw * dh * 4);

            // 为新尺寸更新数据缓冲区
            if (dst_pix_fmt == AV_PIX_FMT_YUV420P) {
                data[0] = (uint8_t*)hframe.buf.base;
                data[1] = data[0] + dw * dh;
                data[2] = data[1] + dw * dh / 4;
                linesize[0] = dw;
                linesize[1] = dw / 2;
                linesize[2] = dw / 2;
            } else if (dst_pix_fmt == AV_PIX_FMT_BGR24) {
                data[0] = (uint8_t*)hframe.buf.base;
                linesize[0] = dw * 3;
            }

            hlogi("sws_ctx 重新创建成功: %dx%d %s -> %dx%d %s",
                  frame->width, frame->height, av_get_pix_fmt_name(actual_fmt),
                  dw, dh, av_get_pix_fmt_name(dst_pix_fmt));
        } else {
            hlogi("帧格式与 sws_ctx 匹配，无需重新创建");
        }
    }

    if (sws_ctx) {
        int h = sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, data, linesize);
        if (h <= 0) {
            hloge("sws_scale 失败! 返回: %d, 帧: %dx%d, 格式: %d(%s)",
                  h, frame->width, frame->height, frame->format,
                  av_get_pix_fmt_name((AVPixelFormat)frame->format));
            return -1;
        }
        if (h != frame->height) {
            hlogw("sws_scale 返回不同高度: %d, 预期: %d (继续)",
                  h, frame->height);
        }
    }

    if (video_time_base_num && video_time_base_den) {
        // 计算原始时间戳(HFFPlayer 不需要速度调整)
        // 速度控制通过调整线程睡眠时间处理
        hframe.ts = frame->pts / (double)video_time_base_den * video_time_base_num * 1000;

        // 更新视频时钟用于音视频同步
        AVRational tb;
        tb.num = video_time_base_num;
        tb.den = video_time_base_den;
        double video_pts = frame->pts * av_q2d(tb);

        // 计算帧延迟(持续时间)
        double delay = video_pts - frame_last_pts;
        if (delay <= 0 || delay > 1.0) {
            // 如果延迟无效，使用最后延迟
            delay = frame_last_delay;
        }

        // 保存给下一帧
        frame_last_pts = video_pts;
        frame_last_delay = delay;

        // 如果不是视频主时钟，为同步调整延迟
        if (get_master_sync_type() != 1) {  // 不是 AV_SYNC_VIDEO_MASTER
            delay = compute_target_delay(delay);
        }

        // 更新帧计时器
        double time = (double)av_gettime_relative() / 1000000.0;
        if (frame_timer == 0) {
            frame_timer = time;
        }
        frame_timer += delay;

        // 实际延迟 - 如果我们领先则睡眠
        double actual_delay = frame_timer - time;
        if (actual_delay > 0 && actual_delay < 1.0) {  // 最大1秒
            int sleep_ms = (int)(actual_delay * 1000);
            if (sleep_ms > 0) {
                msleep(sleep_ms);
            }
        }

        // 更新视频时钟
        time = (double)av_gettime_relative() / 1000000.0;
        set_clock(&video_clock, video_pts, time);
    }

    push_frame(&hframe);
    return 0;
}

int HFFPlayer::processAudioPacket() {
    if (!audio_codec_ctx) {
        return 0;  // 没有音频解码器初始化
    }

    // 如果正在定位，跳过处理
    if (is_seeking.load()) {
        return AVERROR(EAGAIN);
    }

    // 锁定解码器互斥锁以防止与定位操作的竞争条件
    hmutex_lock(&decoder_mutex);

    // 获取锁后再次检查定位标志
    if (is_seeking.load()) {
        hmutex_unlock(&decoder_mutex);
        return AVERROR(EAGAIN);
    }

    int ret = avcodec_send_packet(audio_codec_ctx, packet);
    if (ret != 0) {
        hmutex_unlock(&decoder_mutex);
        hloge("avcodec_send_packet (音频) 错误: %d", ret);
        return ret;
    }

    ret = avcodec_receive_frame(audio_codec_ctx, audio_frame);
    if (ret != 0) {
        hmutex_unlock(&decoder_mutex);
        if (ret == AVERROR(EAGAIN)) {
            return ret;  // 需要更多数据包
        }
        hloge("avcodec_receive_frame (音频) 错误: %d", ret);
        return ret;
    }

    hmutex_unlock(&decoder_mutex);

    // 如果需要，重采样音频
    if (swr_ctx && audio_buffer) {
        int out_samples = swr_convert(swr_ctx,
                                      &audio_buffer, audio_frame->nb_samples,
                                      (const uint8_t**)audio_frame->data, audio_frame->nb_samples);

        if (out_samples > 0) {
            // 以秒计算PTS
            double audio_pts = 0;
            if (audio_frame->pts != AV_NOPTS_VALUE && audio_time_base_num && audio_time_base_den) {
                AVRational atb;
                atb.num = audio_time_base_num;
                atb.den = audio_time_base_den;
                audio_pts = audio_frame->pts * av_q2d(atb);
            }

            // 为队列创建音频帧
            AudioFrame* af = new AudioFrame();
            int data_size = out_samples * audio_channels * sizeof(int16_t);
            af->data = (uint8_t*)av_malloc(data_size);
            if (af->data) {
                memcpy(af->data, audio_buffer, data_size);
                af->size = data_size;
                af->pts = audio_pts;

                // 推送到队列
                hmutex_lock(&audio_queue_mutex);
                audio_frame_queue.push(af);
                size_t queue_size = audio_frame_queue.size();
                hmutex_unlock(&audio_queue_mutex);

                // 调试: 记录前几帧
                static int audio_frame_count = 0;
                if (audio_frame_count < 5) {
                    hlogi("音频帧入队 #%d: 大小=%d, pts=%.3f, 队列大小=%zu",
                          audio_frame_count++, data_size, audio_pts, queue_size);
                }
            } else {
                hloge("分配音频帧缓冲区失败");
                delete af;
            }
        } else {
            hloge("swr_convert 返回0或负采样数: %d", out_samples);
        }
    } else {
        if (!swr_ctx) hloge("swr_ctx 为 NULL");
        if (!audio_buffer) hloge("audio_buffer 为 NULL");
    }

    return 0;
}

void HFFPlayer::doTask() {
    // 循环直到获取视频帧或处理音频
    while (!quit) {
        // av_init_packet 在 FFmpeg 5.x 中已弃用，packet 已由 av_packet_alloc 初始化

        // 锁定 format_mutex 以防止与 seek() 的并发访问
        hmutex_lock(&format_mutex);

        // 检查是否正在定位
        if (is_seeking.load()) {
            hmutex_unlock(&format_mutex);
            msleep(10);  // 等待定位完成
            continue;
        }

        fmt_ctx->interrupt_callback.callback = interrupt_callback;
        fmt_ctx->interrupt_callback.opaque = this;
        block_starttime = time(NULL);
        //hlogi("av_read_frame");
        int ret = av_read_frame(fmt_ctx, packet);
        //hlogi("av_read_frame 返回值=%d", ret);
        fmt_ctx->interrupt_callback.callback = NULL;

        hmutex_unlock(&format_mutex);

        if (ret != 0) {
            hlogi("无帧: %d", ret);
            if (!quit) {
                if (ret == AVERROR_EOF || avio_feof(fmt_ctx->pb)) {
                    eof = 1;
                    event_callback(HPLAYER_EOF);
                }
                else {
                    error = ret;
                    event_callback(HPLAYER_ERROR);
                }
            }
            return;
        }

        // 注意: 如果不调用 av_packet_unref，内存泄漏。
        defer (av_packet_unref(packet);)

            // hlogi("流索引=%d 数据=%p 长度=%d", packet->stream_index, packet->data, packet->size);

            // 处理视频包
            if (packet->stream_index == video_stream_index) {
            ret = processVideoPacket();
            if (ret == 0) {
                // 成功解码视频帧，退出循环
                break;
            }
            else if (ret != AVERROR(EAGAIN)) {
                // 致命错误
                return;
            }
            // EAGAIN 表示需要更多数据包，继续读取
        }
        // 处理音频包
        else if (packet->stream_index == audio_stream_index) {
            ret = processAudioPacket();
            if (ret != 0 && ret != AVERROR(EAGAIN)) {
                // 非致命音频错误，记录并继续
                // 音频处理不阻塞视频播放
            }
            // 继续读取视频帧
        }
        // 跳过其他流
    }
}

// ==================== 时钟函数 ====================
void HFFPlayer::init_clock(Clock* c) {
    c->pts = NAN;
    c->pts_drift = 0;
    c->last_updated = 0;
    c->paused = 0;
}

void HFFPlayer::set_clock(Clock* c, double pts, double time) {
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
}

double HFFPlayer::get_clock(Clock* c) {
    if (c->paused) {
        return c->pts;
    } else {
        double time = (double)av_gettime_relative() / 1000000.0;
        return c->pts_drift + time;
    }
}

int HFFPlayer::get_master_sync_type() {
    // 如果没有音频流，视频必须是主时钟
    if (audio_stream_index < 0 || !audio_codec_ctx || audio_dev_id == 0) {
        return 1;  // AV_SYNC_VIDEO_MASTER
    }

    // 如果没有视频流，音频必须是主时钟
    if (video_stream_index < 0) {
        return 0;  // AV_SYNC_AUDIO_MASTER
    }

    // 否则使用配置的类型
    return av_sync_type;
}

double HFFPlayer::get_master_clock() {
    int sync_type = get_master_sync_type();

    switch (sync_type) {
    case 1:  // AV_SYNC_VIDEO_MASTER
        return get_clock(&video_clock);
    case 2:  // AV_SYNC_EXTERNAL_CLOCK
        // 目前，外部时钟与音频时钟相同
        // 在完整实现中，这将是一个单独的时钟
        return get_clock(&audio_clock);
    case 0:  // AV_SYNC_AUDIO_MASTER
    default:
        return get_clock(&audio_clock);
    }
}

double HFFPlayer::compute_target_delay(double delay) {
    // 参考: ffplay.c compute_target_delay()
    // 此函数调整帧延迟以将视频与主时钟同步

    double sync_threshold, diff = 0;

    // 获取视频时钟和主时钟之间的差异
    diff = get_clock(&video_clock) - get_master_clock();

    // 计算同步阈值
    // AV_SYNC_THRESHOLD_MIN = 0.04, AV_SYNC_THRESHOLD_MAX = 0.1
    const double AV_SYNC_THRESHOLD_MIN = 0.04;
    const double AV_SYNC_THRESHOLD_MAX = 0.1;
    const double AV_SYNC_FRAMEDUP_THRESHOLD = 0.1;
    const double AV_NOSYNC_THRESHOLD = 10.0;

    sync_threshold = (delay > AV_SYNC_THRESHOLD_MAX) ? AV_SYNC_THRESHOLD_MAX :
                         (delay < AV_SYNC_THRESHOLD_MIN) ? AV_SYNC_THRESHOLD_MIN : delay;

    if (!std::isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
        if (diff <= -sync_threshold) {
            // 视频落后于音频，加速(减少延迟)
            delay = (delay + diff < 0) ? 0 : delay + diff;
        } else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
            // 视频领先于音频，减速(增加延迟)
            delay = delay + diff;
        } else if (diff >= sync_threshold) {
            // 视频领先但延迟很小，复制帧(2倍延迟)
            delay = 2 * delay;
        }
    }

    static int log_count = 0;
    if (log_count < 5) {
        hlogi("音视频同步: 差异=%.3f, 延迟=%.3f->%.3f", -diff, frame_last_delay, delay);
        log_count++;
    }

    return delay;
}

// ==================== SDL 音频函数 ====================
void HFFPlayer::sdl_audio_callback(void* userdata, uint8_t* stream, int len) {
    HFFPlayer* player = (HFFPlayer*)userdata;

    static int callback_count = 0;
    static bool first_call = true;

    if (first_call) {
        hlogi("SDL 音频回调首次调用, 长度=%d", len);
        first_call = false;
    }

    memset(stream, 0, len);

    if (player->quit || player->is_seeking.load()) {
        return;
    }

    int total_written = 0;
    while (len > 0) {
        if (player->audio_play_buf_index >= player->audio_play_buf_size) {
            // 需要解码更多音频
            double pts;
            int audio_size = player->audio_decode_frame(&pts);

            if (audio_size < 0) {
                // 错误或无更多帧，输出静音
                if (callback_count < 3) {
                    hlogi("audio_decode_frame 返回 -1 (队列中无帧)");
                }
                player->audio_play_buf = NULL;
                player->audio_play_buf_size = 512;
            } else {
                player->audio_play_buf_size = audio_size;

                if (callback_count < 3) {
                    hlogi("解码音频帧: 大小=%d, pts=%.3f", audio_size, pts);
                }

                // 更新音频时钟
                if (!std::isnan(pts)) {
                    double time = (double)av_gettime_relative() / 1000000.0;
                    player->set_clock(&player->audio_clock, pts, time);
                }
            }
            player->audio_play_buf_index = 0;
        }

        int len1 = player->audio_play_buf_size - player->audio_play_buf_index;
        if (len1 > len)
            len1 = len;

        if (player->audio_play_buf) {
            memcpy(stream, player->audio_play_buf + player->audio_play_buf_index, len1);
            total_written += len1;
        }

        len -= len1;
        stream += len1;
        player->audio_play_buf_index += len1;
    }

    if (callback_count < 3) {
        hlogi("SDL 回调 #%d: 写入 %d 字节", callback_count, total_written);
    }
    callback_count++;
}

int HFFPlayer::audio_decode_frame(double* pts_ptr) {
    hmutex_lock(&audio_queue_mutex);

    if (audio_frame_queue.empty()) {
        hmutex_unlock(&audio_queue_mutex);
        return -1;
    }

    AudioFrame* af = audio_frame_queue.front();
    audio_frame_queue.pop();
    hmutex_unlock(&audio_queue_mutex);

    if (af && af->data && af->size > 0) {
        *pts_ptr = af->pts;
        int size = af->size;

        // 如果需要，分配或重新分配播放缓冲区
        if (!audio_play_buf || size > audio_buffer_size * 2) {
            if (audio_play_buf) {
                av_free(audio_play_buf);
            }
            audio_play_buf = (uint8_t*)av_malloc(size);
            if (!audio_play_buf) {
                delete af;
                return -1;
            }
        }

        // 复制音频数据到播放缓冲区
        memcpy(audio_play_buf, af->data, size);

        delete af;  // 复制后删除
        return size;
    }

    if (af) {
        delete af;
    }
    return -1;
}

int HFFPlayer::synchronize_audio(int nb_samples) {
    // 无速度调整的简单版本
    // 在完整实现中，这将基于音视频差异调整采样率
    return nb_samples;
}

int HFFPlayer::audio_open() {
    // 如果尚未初始化，初始化SDL音频
    static bool sdl_audio_initialized = false;
    if (!sdl_audio_initialized) {
        if (SDL_Init(SDL_INIT_AUDIO) < 0) {
            hloge("无法初始化SDL音频: %s", SDL_GetError());
            return -1;
        }
        sdl_audio_initialized = true;
        hlogi("SDL音频子系统已初始化");
    }

    SDL_AudioSpec wanted_spec, spec;
    memset(&wanted_spec, 0, sizeof(wanted_spec));
    memset(&spec, 0, sizeof(spec));

    wanted_spec.freq = audio_sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = audio_channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = 1024;  // 缓冲区大小
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = this;

    hlogi("打开SDL音频: 频率=%d, 声道数=%d, 格式=S16",
          wanted_spec.freq, wanted_spec.channels);

    audio_dev_id = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);

    if (audio_dev_id == 0) {
        hloge("打开音频设备失败: %s", SDL_GetError());
        return -1;
    }

    audio_hw_buf_size = spec.size;

    hlogi("SDL音频成功打开:");
    hlogi("  设备 ID: %d", audio_dev_id);
    hlogi("  频率: %d Hz (期望 %d)", spec.freq, wanted_spec.freq);
    hlogi("  声道数: %d (期望 %d)", spec.channels, wanted_spec.channels);
    hlogi("  采样数: %d", spec.samples);
    hlogi("  缓冲区大小: %d 字节", spec.size);

    // 开始音频播放
    SDL_PauseAudioDevice(audio_dev_id, 0);
    hlogi("SDL音频播放已开始");

    return 0;
}

void HFFPlayer::audio_close() {
    if (audio_dev_id) {
        SDL_PauseAudioDevice(audio_dev_id, 1);  // 首先暂停
        SDL_CloseAudioDevice(audio_dev_id);
        audio_dev_id = 0;
    }

    // 清除音频队列
    hmutex_lock(&audio_queue_mutex);
    while (!audio_frame_queue.empty()) {
        AudioFrame* af = audio_frame_queue.front();
        audio_frame_queue.pop();
        delete af;
    }
    hmutex_unlock(&audio_queue_mutex);

    // 释放播放缓冲区
    if (audio_play_buf) {
        av_free(audio_play_buf);
        audio_play_buf = NULL;
    }
    audio_play_buf_size = 0;
    audio_play_buf_index = 0;
}
