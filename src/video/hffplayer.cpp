#include "hffplayer.h"

#include "confile.h"
#include "hlog.h"
#include "hstring.h"
#include "hscope.h"
#include "htime.h"

#include <cstring>

#define DEFAULT_BLOCK_TIMEOUT   10  // s

std::atomic_flag HFFPlayer::s_ffmpeg_init = ATOMIC_FLAG_INIT;

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

static void debug_all_hardware_decoders() {
    const AVCodec* codec = NULL;
    void* iter = NULL;
    
    hlogi("=== Available Hardware Decoders ===");
    hlogi("FFmpeg version: %s", av_version_info());
    
    // List all hardware device types
    hlogi("\n--- Supported Hardware Device Types ---");
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
        hlogi("  Hardware device: %s", av_hwdevice_get_type_name(type));
    }
    
    // List all hardware decoders
    hlogi("\n--- Available Hardware Decoders ---");
    int hw_decoder_count = 0;
    while ((codec = av_codec_iterate(&iter))) {
        if (av_codec_is_decoder(codec)) {
            // Check if it's a hardware decoder by name suffix
            const char* name = codec->name;
            bool is_hw = false;
            
            if (strstr(name, "_cuvid") || strstr(name, "_qsv") || 
                strstr(name, "_dxva2") || strstr(name, "_d3d11va") ||
                strstr(name, "_videotoolbox") || strstr(name, "_vaapi") ||
                strstr(name, "_vdpau") || strstr(name, "_mediacodec")) {
                is_hw = true;
            }
            
            // Also check hardware capability flag
            if (codec->capabilities & AV_CODEC_CAP_HARDWARE) {
                is_hw = true;
            }
            
            if (is_hw) {
                hw_decoder_count++;
                hlogi("  [%d] Hardware decoder: %-25s (%s)", 
                      hw_decoder_count, codec->name, codec->long_name);
                
                // Check supported hardware configurations
                for (int i = 0; ; i++) {
                    const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
                    if (!config) break;
                    
                    const char* hw_type = av_hwdevice_get_type_name(config->device_type);
                    if (hw_type) {
                        hlogi("      -> Supports device: %s (method: %d)", 
                              hw_type, config->methods);
                    }
                }
            }
        }
    }
    
    if (hw_decoder_count == 0) {
        hlogi("  No hardware decoders found!");
        hlogi("  Note: You may need to recompile FFmpeg with hardware acceleration support");
    } else {
        hlogi("\nTotal hardware decoders found: %d", hw_decoder_count);
    }
    
    hlogi("=== End of Hardware Decoder List ===\n");
}

// NOTE: avformat_open_input,av_read_frame block
static int interrupt_callback(void* opaque) {
    if (opaque == NULL) return 0;
    HFFPlayer* player = (HFFPlayer*)opaque;
    if (player->quit ||
        time(NULL) - player->block_starttime > player->block_timeout) {
        hlogi("interrupt quit=%d media.src=%s", player->quit, player->media.src.c_str());
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

    block_starttime = time(NULL);
    block_timeout = DEFAULT_BLOCK_TIMEOUT;
    quit = 0;
    
    // Initialize thread synchronization
    hmutex_init(&decoder_mutex);
    is_seeking.store(false);


    if (!s_ffmpeg_init.test_and_set()) {
        // av_register_all();
        // avcodec_register_all();
        avformat_network_init();
        avdevice_register_all();
        list_devices();
        
        // Debug: List all available hardware decoders
        debug_all_hardware_decoders();
    }
}

HFFPlayer::~HFFPlayer() {
    close();
    hmutex_destroy(&decoder_mutex);
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
            hloge("Can not find dshow");
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

    hlogi("ifile:%s", ifile.c_str());
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
        av_dict_set(&fmt_opts, "stimeout", "5000000", 0);   // us
    }
    av_dict_set(&fmt_opts, "buffer_size", "2048000", 0);
    fmt_ctx->interrupt_callback.callback = interrupt_callback;
    fmt_ctx->interrupt_callback.opaque = this;
    block_starttime = time(NULL);
    ret = avformat_open_input(&fmt_ctx, ifile.c_str(), (AVInputFormat*)ifmt, &fmt_opts);
    if (ret != 0) {
        hloge("Open input file[%s] failed: %d", ifile.c_str(), ret);
        return ret;
    }
    fmt_ctx->interrupt_callback.callback = NULL;
    defer (if (ret != 0 && fmt_ctx) {avformat_close_input(&fmt_ctx);})

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret != 0) {
        hloge("Can not find stream: %d", ret);
        return ret;
    }
    hlogi("stream_num=%d", fmt_ctx->nb_streams);

    video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    audio_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    subtitle_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
    hlogi("video_stream_index=%d", video_stream_index);
    hlogi("audio_stream_index=%d", audio_stream_index);
    hlogi("subtitle_stream_index=%d", subtitle_stream_index);

    if (video_stream_index < 0) {
        hloge("Can not find video stream.");
        ret = -20;
        return ret;
    }

    AVStream* video_stream = fmt_ctx->streams[video_stream_index];
    video_time_base_num = video_stream->time_base.num;
    video_time_base_den = video_stream->time_base.den;
    hlogi("video_stream time_base=%d/%d", video_stream->time_base.num, video_stream->time_base.den);

    AVCodecParameters* codec_param = video_stream->codecpar;
    hlogi("codec_id=%d:%s", codec_param->codec_id, avcodec_get_name(codec_param->codec_id));

    const AVCodec* codec = NULL;
    if (decode_mode != SOFTWARE_DECODE) {
try_hardware_decode:
        std::string codec_name(avcodec_get_name(codec_param->codec_id));
        std::string decoder_name;
        bool found = false;
        
        if (decode_mode == HARDWARE_DECODE_AUTO) {
            // Auto mode: try all hardware decoders by priority
            // Windows priority: CUVID(NVIDIA) > QSV(Intel) > D3D11VA > DXVA2
            const char* hw_suffixes[] = {"_cuvid", "_qsv", "_d3d11va", "_dxva2"};
            const char* hw_names[] = {"NVIDIA CUVID", "Intel QSV", "D3D11VA", "DXVA2"};
            const int hw_modes[] = {HARDWARE_DECODE_CUVID, HARDWARE_DECODE_QSV, 
                                    HARDWARE_DECODE_D3D11VA, HARDWARE_DECODE_DXVA2};
            
            for (int i = 0; i < 4; ++i) {
                decoder_name = codec_name + hw_suffixes[i];
                codec = avcodec_find_decoder_by_name(decoder_name.c_str());
                
                if (codec != NULL) {
                    real_decode_mode = hw_modes[i];
                    hlogi("Found hardware decoder: %s (%s)", decoder_name.c_str(), hw_names[i]);
                    found = true;
                    break;
                } else {
                    hlogi("Hardware decoder not available: %s (%s)", decoder_name.c_str(), hw_names[i]);
                }
            }
        } else {
            // Manual mode: try specific decoder
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
                    hlogi("Found hardware decoder: %s (%s)", decoder_name.c_str(), hw_name);
                    found = true;
                } else {
                    hlogi("Hardware decoder not available: %s (%s)", decoder_name.c_str(), hw_name);
                }
            }
        }
        
        if (!found) {
            hlogi("No hardware decoder found, will try software decode");
        }
    }

    if (codec == NULL) {
try_software_decode:
        codec = avcodec_find_decoder(codec_param->codec_id);
        if (codec == NULL) {
            hloge("Can not find decoder %s", avcodec_get_name(codec_param->codec_id));
            ret = -30;
            return ret;
        }
        real_decode_mode = SOFTWARE_DECODE;
        hlogi("Using software decoder: %s", codec->name);
    }

    hlogi("codec_name: %s=>%s", codec->name, codec->long_name);

    codec_ctx = avcodec_alloc_context3(codec);
    if (codec_ctx == NULL) {
        hloge("avcodec_alloc_context3");
        ret = -40;
        return ret;
    }
    defer (if (ret != 0 && codec_ctx) {avcodec_free_context(&codec_ctx); codec_ctx = NULL;})

    ret = avcodec_parameters_to_context(codec_ctx, codec_param);
    if (ret != 0) {
        hloge("avcodec_parameters_to_context error: %d", ret);
        return ret;
    }

    if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        av_dict_set(&codec_opts, "refcounted_frames", "1", 0);
    }
    ret = avcodec_open2(codec_ctx, codec, &codec_opts);
    if (ret != 0) {
        if (real_decode_mode != SOFTWARE_DECODE) {
            hlogi("Can not open hardware codec error: %d, try software codec.", ret);
            // Clean up failed hardware decoder context
            avcodec_free_context(&codec_ctx);
            codec_ctx = NULL;
            // Find software decoder
            codec = avcodec_find_decoder(codec_param->codec_id);
            if (codec == NULL) {
                hloge("Can not find software decoder %s", avcodec_get_name(codec_param->codec_id));
                ret = -30;
                return ret;
            }
            real_decode_mode = SOFTWARE_DECODE;
            hlogi("Using software decoder: %s", codec->name);
            
            // Reallocate decoder context
            codec_ctx = avcodec_alloc_context3(codec);
            if (codec_ctx == NULL) {
                hloge("avcodec_alloc_context3");
                ret = -40;
                return ret;
            }
            
            ret = avcodec_parameters_to_context(codec_ctx, codec_param);
            if (ret != 0) {
                hloge("avcodec_parameters_to_context error: %d", ret);
                return ret;
            }
            
            ret = avcodec_open2(codec_ctx, codec, &codec_opts);
            if (ret != 0) {
                hloge("Can not open software codec error: %d", ret);
                return ret;
            }
        } else {
            hloge("Can not open software codec error: %d", ret);
            return ret;
        }
    }
    video_stream->discard = AVDISCARD_DEFAULT;

    int sw, sh, dw, dh;
    sw = codec_ctx->width;
    sh = codec_ctx->height;
    src_pix_fmt = codec_ctx->pix_fmt;
    hlogi("sw=%d sh=%d src_pix_fmt=%d:%s", sw, sh, src_pix_fmt, av_get_pix_fmt_name(src_pix_fmt));
    if (sw <= 0 || sh <= 0 || src_pix_fmt == AV_PIX_FMT_NONE) {
        hloge("Codec parameters wrong!");
        ret = -45;
        return ret;
    }

    dw = sw >> 2 << 2; // align = 4
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
    hlogi("dw=%d dh=%d dst_pix_fmt=%d:%s", dw, dh, dst_pix_fmt, av_get_pix_fmt_name(dst_pix_fmt));

    sws_ctx = sws_getContext(sw, sh, src_pix_fmt, dw, dh, dst_pix_fmt, SWS_BICUBIC, NULL, NULL, NULL);
    if (sws_ctx == NULL) {
        hloge("sws_getContext");
        ret = -50;
        return ret;
    }
    
    // Reset flag to check format on first decoded frame
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

    // HVideoPlayer member vars
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
    hlogi("fps=%d duration=%lldms start_time=%lldms", fps, duration, start_time);

    // Initialize audio decoder if audio stream exists
    if (audio_stream_index >= 0) {
        AVStream* audio_stream = fmt_ctx->streams[audio_stream_index];
        audio_time_base_num = audio_stream->time_base.num;
        audio_time_base_den = audio_stream->time_base.den;
        hlogi("audio_stream time_base=%d/%d", audio_stream->time_base.num, audio_stream->time_base.den);

        AVCodecParameters* audio_codec_param = audio_stream->codecpar;
        hlogi("audio_codec_id=%d:%s", audio_codec_param->codec_id, avcodec_get_name(audio_codec_param->codec_id));
        
        const AVCodec* audio_codec = avcodec_find_decoder(audio_codec_param->codec_id);
        if (audio_codec == NULL) {
            hloge("Can not find audio decoder %s", avcodec_get_name(audio_codec_param->codec_id));
            // Audio is optional, continue without it
        } else {
            hlogi("audio_codec_name: %s=>%s", audio_codec->name, audio_codec->long_name);
            
            audio_codec_ctx = avcodec_alloc_context3(audio_codec);
            if (audio_codec_ctx == NULL) {
                hloge("avcodec_alloc_context3 for audio failed");
            } else {
                ret = avcodec_parameters_to_context(audio_codec_ctx, audio_codec_param);
                if (ret != 0) {
                    hloge("avcodec_parameters_to_context for audio error: %d", ret);
                    avcodec_free_context(&audio_codec_ctx);
                    audio_codec_ctx = NULL;
                } else {
                    ret = avcodec_open2(audio_codec_ctx, audio_codec, NULL);
                    if (ret != 0) {
                        hloge("Can not open audio codec error: %d", ret);
                        avcodec_free_context(&audio_codec_ctx);
                        audio_codec_ctx = NULL;
                    } else {
                        audio_stream->discard = AVDISCARD_DEFAULT;
                        audio_frame = av_frame_alloc();
                        
                        // Initialize audio resampler for PCM S16LE stereo output
                        audio_channels = 2;
                        audio_sample_rate = 44100;
                        
                        // Use newer channel layout API if available
                        int64_t in_ch_layout = AV_CH_LAYOUT_STEREO;
                        if (audio_codec_ctx->channel_layout) {
                            in_ch_layout = audio_codec_ctx->channel_layout;
                        } else if (audio_codec_ctx->channels > 0) {
                            in_ch_layout = av_get_default_channel_layout(audio_codec_ctx->channels);
                        }
                        
                        swr_ctx = swr_alloc_set_opts(NULL,
                            AV_CH_LAYOUT_STEREO,          // out_ch_layout
                            AV_SAMPLE_FMT_S16,            // out_sample_fmt
                            audio_sample_rate,            // out_sample_rate
                            in_ch_layout,                 // in_ch_layout
                            audio_codec_ctx->sample_fmt,  // in_sample_fmt
                            audio_codec_ctx->sample_rate, // in_sample_rate
                            0, NULL);
                        
                        if (swr_ctx) {
                            ret = swr_init(swr_ctx);
                            if (ret < 0) {
                                hloge("swr_init failed: %d", ret);
                                swr_free(&swr_ctx);
                                swr_ctx = NULL;
                            } else {
                                // Allocate audio output buffer
                                audio_buffer_size = av_samples_get_buffer_size(NULL, audio_channels, 
                                    audio_codec_ctx->frame_size > 0 ? audio_codec_ctx->frame_size : 1024, 
                                    AV_SAMPLE_FMT_S16, 1);
                                audio_buffer = (uint8_t*)av_malloc(audio_buffer_size);
                                
                                hlogi("Audio decoder initialized: channels=%d, sample_rate=%d", 
                                    audio_channels, audio_sample_rate);
                            }
                        }
                    }
                }
            }
        }
    }

    HThread::setSleepPolicy(HThread::SLEEP_UNTIL, 1000 / fps);
    return ret;
}

int HFFPlayer::close() {
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
    // NOTE: This function should be called with decoder_mutex locked
    // Flush video decoder
    if (codec_ctx) {
        avcodec_flush_buffers(codec_ctx);
        hlogi("Video decoder flushed");
    }
    
    // Flush audio decoder
    if (audio_codec_ctx) {
        avcodec_flush_buffers(audio_codec_ctx);
        hlogi("Audio decoder flushed");
    }
}

int HFFPlayer::seek(int64_t ms) {
    // Default: fast seek (to keyframe)
    return seek(ms, false);
}

int HFFPlayer::seek(int64_t ms, bool accurate) {
    if (!fmt_ctx) {
        hloge("seek failed: fmt_ctx is NULL");
        return -1;
    }
    
    const char* seek_mode = accurate ? "accurate" : "fast";
    hlogi("seek=>%lldms (mode=%s, duration=%lldms, start_time=%lldms)", 
          ms, seek_mode, duration, start_time);
    
    // Check if seek position is valid
    if (ms < 0) {
        hlogw("seek position is negative, clamping to 0");
        ms = 0;
    }
    
    if (duration > 0 && ms > duration) {
        hlogw("seek position exceeds duration, clamping to duration");
        ms = duration;
    }
    
    // Set seeking flag to prevent decoder operations in worker thread
    is_seeking.store(true);
    
    // Clear frame cache before seeking
    clear_frame_cache();
    
    // Reset EOF flag
    eof = 0;
    error = 0;
    
    int ret = 0;
    int64_t seek_target = 0;
    int seek_flags = AVSEEK_FLAG_BACKWARD;
    
    // For accurate seek, we'll still seek to keyframe but decode frames until target
    // For fast seek, we just seek to nearest keyframe
    if (accurate) {
        // In accurate mode, we may want to seek slightly before target
        // to ensure we can decode to exact frame
        hlogi("Accurate seek mode: will decode to exact position");
    }
    
    // Try to seek using video stream timestamp (more accurate)
    if (video_stream_index >= 0 && video_time_base_num && video_time_base_den) {
        // Calculate target timestamp in stream timebase
        seek_target = (start_time + ms) / 1000.0 / video_time_base_num * video_time_base_den;
        
        hlogi("Seeking video stream: target_ms=%lld, start_time=%lld, timestamp=%lld", 
              ms, start_time, seek_target);
        
        ret = av_seek_frame(fmt_ctx, video_stream_index, seek_target, seek_flags);
        
        if (ret < 0) {
            hloge("av_seek_frame failed for video stream: %d", ret);
            // Try seeking without stream index (let FFmpeg choose the best stream)
            seek_target = (start_time + ms) * 1000; // in microseconds (AV_TIME_BASE)
            ret = av_seek_frame(fmt_ctx, -1, seek_target, seek_flags);
            
            if (ret < 0) {
                hloge("av_seek_frame failed for any stream: %d", ret);
                is_seeking.store(false);  // Clear flag before returning
                return ret;
            } else {
                hlogi("Seek succeeded using default stream selection");
            }
        } else {
            hlogi("Video stream seek succeeded");
        }
    } else {
        // No video stream or timebase not set, use default timestamp
        seek_target = (start_time + ms) * 1000; // in microseconds (AV_TIME_BASE)
        hlogi("Seeking with default timebase: target_ms=%lld, timestamp=%lld", ms, seek_target);
        
        ret = av_seek_frame(fmt_ctx, -1, seek_target, seek_flags);
        
        if (ret < 0) {
            hloge("av_seek_frame failed: %d", ret);
            is_seeking.store(false);  // Clear flag before returning
            return ret;
        }
    }
    
    // Lock decoder mutex before flushing
    hmutex_lock(&decoder_mutex);
    
    // Flush decoders to clear buffered frames
    flushDecoders();
    
    hmutex_unlock(&decoder_mutex);
    
    // Optionally seek audio stream if it exists and has different timebase
    if (audio_stream_index >= 0 && audio_time_base_num && audio_time_base_den) {
        // Audio stream will be synchronized automatically during playback
        // But we can log the expected audio timestamp
        int64_t audio_seek_target = (start_time + ms) / 1000.0 / audio_time_base_num * audio_time_base_den;
        hlogi("Audio stream expected timestamp: %lld", audio_seek_target);
    }
    
    // Clear seeking flag
    is_seeking.store(false);
    
    hlogi("Seek completed successfully to %lldms (mode=%s)", ms, seek_mode);
    return 0;
}

int HFFPlayer::seekByPercent(double percent) {
    if (duration <= 0) {
        hloge("seekByPercent failed: duration is not available");
        return -1;
    }
    
    // Clamp percent to valid range
    if (percent < 0.0) {
        percent = 0.0;
    } else if (percent > 100.0) {
        percent = 100.0;
    }
    
    // Calculate target position in milliseconds
    int64_t target_ms = (int64_t)(duration * percent / 100.0);
    
    hlogi("seekByPercent: %.2f%% => %lldms (duration=%lldms)", percent, target_ms, duration);
    
    return seek(target_ms);
}

int HFFPlayer::seekRelative(int64_t offset_ms) {
    int64_t current_pos = getCurrentPosition();
    
    if (current_pos < 0) {
        hloge("seekRelative failed: cannot determine current position");
        return -1;
    }
    
    int64_t target_ms = current_pos + offset_ms;
    
    hlogi("seekRelative: current=%lldms, offset=%lldms => target=%lldms", 
          current_pos, offset_ms, target_ms);
    
    return seek(target_ms);
}

int64_t HFFPlayer::getCurrentPosition() {
    // Use the timestamp from the last decoded frame
    if (hframe.ts >= 0) {
        // hframe.ts is already in milliseconds
        return hframe.ts;
    }
    
    // If no frame has been decoded yet, return 0
    return 0;
}

void HFFPlayer::set_speed(double speed) {
    // Call base class to set playback_speed
    HVideoPlayer::set_speed(speed);
    
    // CRITICAL: Adjust decode thread sleep time for HFFPlayer architecture
    // Unlike ffplay which uses Clock in display loop, HFFPlayer controls
    // frame rate via thread sleep policy
    if (fps > 0 && speed > 0.0) {
        int sleep_ms = (int)((1000.0 / fps) / speed);
        if (sleep_ms < 1) sleep_ms = 1;  // Minimum 1ms
        HThread::setSleepPolicy(HThread::SLEEP_UNTIL, sleep_ms);
        hlogi("Playback speed set to %.2fx (decode sleep: %dms)", speed, sleep_ms);
    } else {
        hlogi("Playback speed set to %.2fx (no sleep adjustment)", speed);
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
    // Skip processing if seeking is in progress
    if (is_seeking.load()) {
        return AVERROR(EAGAIN);
    }
    
    // Lock decoder mutex to prevent race condition with seek operation
    hmutex_lock(&decoder_mutex);
    
    // Double check seeking flag after acquiring lock
    if (is_seeking.load()) {
        hmutex_unlock(&decoder_mutex);
        return AVERROR(EAGAIN);
    }
    
    int ret = avcodec_send_packet(codec_ctx, packet);
    if (ret != 0) {
        hmutex_unlock(&decoder_mutex);
        hloge("avcodec_send_packet error: %d", ret);
        return ret;
    }
    
    ret = avcodec_receive_frame(codec_ctx, frame);
    if (ret != 0) {
        hmutex_unlock(&decoder_mutex);
        if (ret == AVERROR(EAGAIN)) {
            return ret;  // Need more packets
        }
        hloge("avcodec_receive_frame error: %d", ret);
        return ret;
    }
    
    hmutex_unlock(&decoder_mutex);

    // On first decoded frame, verify/recreate sws_ctx with actual frame format
    if (!sws_ctx_checked) {
        sws_ctx_checked = true;
        
        AVPixelFormat actual_fmt = (AVPixelFormat)frame->format;
        
        // Check if frame format matches what sws_ctx expects
        if (actual_fmt != src_pix_fmt || frame->width != width || frame->height != height) {
            hlogi("Frame format mismatch detected! Recreating sws_ctx...");
            hlogi("Expected: %dx%d fmt=%s(%d)", width, height, 
                  av_get_pix_fmt_name(src_pix_fmt), src_pix_fmt);
            hlogi("Actual: %dx%d fmt=%s(%d)", frame->width, frame->height,
                  av_get_pix_fmt_name(actual_fmt), actual_fmt);
            
            // Free old sws_ctx
            if (sws_ctx) {
                sws_freeContext(sws_ctx);
                sws_ctx = NULL;
            }
            
            // Update stored values
            width = frame->width;
            height = frame->height;
            src_pix_fmt = actual_fmt;
            
            // Recreate with actual frame parameters
            int dw = frame->width >> 2 << 2;  // align to 4
            int dh = frame->height;
            
            sws_ctx = sws_getContext(
                frame->width, frame->height, actual_fmt,  // actual source
                dw, dh, dst_pix_fmt,                      // destination
                SWS_BICUBIC, NULL, NULL, NULL);
            
            if (!sws_ctx) {
                hloge("Failed to recreate sws_ctx!");
                return -1;
            }
            
            // Update hframe dimensions
            hframe.w = dw;
            hframe.h = dh;
            hframe.buf.resize(dw * dh * 4);
            
            // Update data buffers for new dimensions
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
            
            hlogi("sws_ctx recreated successfully: %dx%d %s -> %dx%d %s",
                  frame->width, frame->height, av_get_pix_fmt_name(actual_fmt),
                  dw, dh, av_get_pix_fmt_name(dst_pix_fmt));
        } else {
            hlogi("Frame format matches sws_ctx, no recreation needed");
        }
    }

    if (sws_ctx) {
        int h = sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, data, linesize);
        if (h <= 0) {
            hloge("sws_scale failed! returned: %d, frame: %dx%d, format: %d(%s)", 
                  h, frame->width, frame->height, frame->format,
                  av_get_pix_fmt_name((AVPixelFormat)frame->format));
            return -1;
        }
        if (h != frame->height) {
            hlogw("sws_scale returned different height: %d, expected: %d (continuing anyway)", 
                  h, frame->height);
        }
    }

    if (video_time_base_num && video_time_base_den) {
        // Calculate original timestamp (no speed adjustment needed for HFFPlayer)
        // Speed control is handled by adjusting thread sleep time
        hframe.ts = frame->pts / (double)video_time_base_den * video_time_base_num * 1000;
    }

    push_frame(&hframe);
    return 0;
}

int HFFPlayer::processAudioPacket() {
    if (!audio_codec_ctx) {
        return 0;  // No audio decoder initialized
    }
    
    // Skip processing if seeking is in progress
    if (is_seeking.load()) {
        return AVERROR(EAGAIN);
    }
    
    // Lock decoder mutex to prevent race condition with seek operation
    hmutex_lock(&decoder_mutex);
    
    // Double check seeking flag after acquiring lock
    if (is_seeking.load()) {
        hmutex_unlock(&decoder_mutex);
        return AVERROR(EAGAIN);
    }

    int ret = avcodec_send_packet(audio_codec_ctx, packet);
    if (ret != 0) {
        hmutex_unlock(&decoder_mutex);
        hloge("avcodec_send_packet (audio) error: %d", ret);
        return ret;
    }
    
    ret = avcodec_receive_frame(audio_codec_ctx, audio_frame);
    if (ret != 0) {
        hmutex_unlock(&decoder_mutex);
        if (ret == AVERROR(EAGAIN)) {
            return ret;  // Need more packets
        }
        hloge("avcodec_receive_frame (audio) error: %d", ret);
        return ret;
    }
    
    hmutex_unlock(&decoder_mutex);

    // Resample audio if needed
    if (swr_ctx && audio_buffer) {
        int out_samples = swr_convert(swr_ctx,
            &audio_buffer, audio_frame->nb_samples,
            (const uint8_t**)audio_frame->data, audio_frame->nb_samples);
        
        if (out_samples > 0) {
            int64_t audio_ts = 0;
            if (audio_time_base_num && audio_time_base_den) {
                audio_ts = audio_frame->pts / (double)audio_time_base_den * audio_time_base_num * 1000;
            }
            
            // Here you can process the audio data (audio_buffer)
            // For example: send to audio device, save to file, etc.
            // hlogi("Audio frame decoded: pts=%lld samples=%d", audio_ts, out_samples);
        }
    }

    return 0;
}

void HFFPlayer::doTask() {
    // loop until get a video frame or process audio
    while (!quit) {
        // av_init_packet is deprecated in FFmpeg 5.x, packet is already initialized by av_packet_alloc

        fmt_ctx->interrupt_callback.callback = interrupt_callback;
        fmt_ctx->interrupt_callback.opaque = this;
        block_starttime = time(NULL);
        //hlogi("av_read_frame");
        int ret = av_read_frame(fmt_ctx, packet);
        //hlogi("av_read_frame retval=%d", ret);
        fmt_ctx->interrupt_callback.callback = NULL;
        if (ret != 0) {
            hlogi("No frame: %d", ret);
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

        // NOTE: if not call av_packet_unref, memory leak.
        defer (av_packet_unref(packet);)

        // hlogi("stream_index=%d data=%p len=%d", packet->stream_index, packet->data, packet->size);
        
        // Process video packet
        if (packet->stream_index == video_stream_index) {
            ret = processVideoPacket();
            if (ret == 0) {
                // Successfully decoded a video frame, exit loop
                break;
            }
            else if (ret != AVERROR(EAGAIN)) {
                // Fatal error
                return;
            }
            // EAGAIN means need more packets, continue reading
        }
        // Process audio packet
        else if (packet->stream_index == audio_stream_index) {
            ret = processAudioPacket();
            if (ret != 0 && ret != AVERROR(EAGAIN)) {
                // Non-fatal audio error, log and continue
                // Audio processing doesn't block video playback
            }
            // Continue reading for video frame
        }
        // Skip other streams
    }
}
