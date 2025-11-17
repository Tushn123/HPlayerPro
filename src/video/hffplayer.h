#ifndef H_FFPLAYER_H
#define H_FFPLAYER_H

#include "HVideoPlayer.h"
#include "ffmpeg_util.h"

#include "hthread.h"
#include "hmutex.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <SDL2/SDL.h>
#include <queue>

// Clock for A/V synchronization
struct Clock {
    double pts;
    double pts_drift;
    double last_updated;
    int paused;
    
    Clock() : pts(0), pts_drift(0), last_updated(0), paused(0) {}
};

// Audio frame for queue
struct AudioFrame {
    uint8_t* data;
    int size;
    double pts;
    
    AudioFrame() : data(nullptr), size(0), pts(0) {}
    ~AudioFrame() {
        if (data) {
            av_free(data);
        }
    }
};

// Packet queue for multi-threaded decoding (C++11 standard library)
struct PacketQueue {
    std::queue<AVPacket*> packets;
    std::mutex mutex;                    // C++11 标准互斥锁
    std::condition_variable cond;        // C++11 标准条件变量
    int nb_packets;                      // Number of packets in queue
    int64_t size;                        // Total size in bytes
    int64_t duration;                    // Total duration in stream timebase
    std::atomic<bool> abort_request;     // Flag to abort blocking operations
    
    // Statistics (ffplay-style)
    int max_nb_packets;                  // Peak packet count
    int64_t max_size;                    // Peak size in bytes
    uint64_t total_packets_put;          // Total packets ever put
    uint64_t total_packets_get;          // Total packets ever got
    
    PacketQueue() : nb_packets(0), size(0), duration(0), abort_request(false),
                    max_nb_packets(0), max_size(0), 
                    total_packets_put(0), total_packets_get(0) {
    }
};

class HFFPlayer : public HVideoPlayer, public HThread {
public:
    HFFPlayer();
    ~HFFPlayer();

    virtual int start() {
        quit = 0;
        return HThread::start();
    }
    virtual int stop() {
        quit = 1;
        return HThread::stop();
    }
    virtual int pause() {return HThread::pause();}
    virtual int resume() {return HThread::resume();}

    // Seek functions
    virtual int seek(int64_t ms);             // Seek to absolute position (fast mode)
    virtual int seek(int64_t ms, bool accurate); // Seek with mode selection
    
    // Additional seek utilities
    int seekByPercent(double percent);        // Seek by percentage (0.0 - 100.0)
    int seekRelative(int64_t offset_ms);      // Seek relative to current position (e.g., +5000 or -5000)
    int64_t getCurrentPosition();             // Get current playback position in ms
    
    // Playback speed control (overrides base class to add timestamp adjustment)
    virtual void set_speed(double speed) override;

private:
    virtual bool doPrepare();
    virtual void doTask();      // Video decoding thread
    virtual bool doFinish();

    int open();
    int close();
    
    // Multi-threaded architecture
    void readTask();            // Packet reading thread (demuxer)
    void audioTask();           // Audio decoding thread

public:
    int64_t block_starttime;
    int64_t block_timeout;
    int     quit;
    string version;

private:
    static std::atomic_flag s_ffmpeg_init;

    AVDictionary*       fmt_opts;
    AVDictionary*       codec_opts;
    AVFormatContext*    fmt_ctx;
    AVPacket*           packet;
    int                 subtitle_stream_index;  // 字幕流索引（未封装）

    // ==================== 内部上下文结构体（方案1封装）====================
    
    // 视频解码上下文：封装所有视频相关变量
    struct VideoContext {
        AVCodecContext* codec_ctx;
        AVFrame* frame;
        SwsContext* sws_ctx;
        AVPixelFormat src_pix_fmt;
        AVPixelFormat dst_pix_fmt;
        uint8_t* data[4];
        int linesize[4];
        HFrame hframe;
        bool sws_ctx_checked;
        int stream_index;
        int time_base_num;
        int time_base_den;
        
        VideoContext() 
            : codec_ctx(nullptr), frame(nullptr), sws_ctx(nullptr),
              src_pix_fmt(AV_PIX_FMT_NONE), dst_pix_fmt(AV_PIX_FMT_NONE),
              sws_ctx_checked(false), stream_index(-1),
              time_base_num(0), time_base_den(0) {
            memset(data, 0, sizeof(data));
            memset(linesize, 0, sizeof(linesize));
        }
    };
    
    // 音频播放上下文：封装所有音频相关变量
    struct AudioContext {
        AVCodecContext* codec_ctx;
        AVFrame* frame;
        SwrContext* swr_ctx;
        uint8_t* buffer;
        int buffer_size;
        int channels;
        int sample_rate;
        SDL_AudioDeviceID dev_id;
        int hw_buf_size;
        std::queue<AudioFrame*> frame_queue;
        std::mutex queue_mutex;
        uint8_t* play_buf;
        int play_buf_size;
        int play_buf_index;
        int stream_index;
        int time_base_num;
        int time_base_den;
        
        AudioContext()
            : codec_ctx(nullptr), frame(nullptr), swr_ctx(nullptr),
              buffer(nullptr), buffer_size(0), channels(0), sample_rate(0),
              dev_id(0), hw_buf_size(0), play_buf(nullptr),
              play_buf_size(0), play_buf_index(0),
              stream_index(-1), time_base_num(0), time_base_den(0) {}
    };
    
    // 同步上下文：封装所有A/V同步相关变量
    struct SyncContext {
        Clock audio_clock;
        Clock video_clock;
        int av_sync_type;           // 0=audio master, 1=video master, 2=external
        double audio_diff_cum;
        double audio_diff_avg_coef;
        int audio_diff_avg_count;
        double frame_timer;
        double frame_last_pts;
        double frame_last_delay;
        bool first_frame;           // 首帧标志（修复第二次播放bug）
        
        SyncContext()
            : av_sync_type(0), audio_diff_cum(0),
              audio_diff_avg_coef(0), audio_diff_avg_count(0),
              frame_timer(0), frame_last_pts(0), frame_last_delay(0),
              first_frame(true) {}
    };
    
    // 线程上下文：封装所有多线程相关变量
    struct ThreadContext {
        PacketQueue video_packet_queue;
        PacketQueue audio_packet_queue;
        std::thread* read_thread;
        std::thread* audio_thread;
        std::atomic<bool> read_thread_running;
        std::atomic<bool> audio_thread_running;
        std::mutex decoder_mutex;
        std::mutex format_mutex;
        std::atomic<bool> is_seeking;
        
        ThreadContext()
            : read_thread(nullptr), audio_thread(nullptr),
              read_thread_running(false), audio_thread_running(false),
              is_seeking(false) {}
    };
    
    // ==================== 上下文实例 ====================
    VideoContext video_;    // 视频上下文
    AudioContext audio_;    // 音频上下文
    SyncContext sync_;      // 同步上下文
    ThreadContext thread_;  // 线程上下文

    // ==================== 成员函数 ====================
    // processing functions
    int processVideoPacket(AVPacket* pkt);
    int processAudioPacket(AVPacket* pkt);
    
    // PacketQueue management functions (ffplay-style with condition variable)
    int packet_queue_put(PacketQueue* q, AVPacket* pkt);
    int packet_queue_get(PacketQueue* q, AVPacket* pkt, bool block);
    void packet_queue_flush(PacketQueue* q);
    void packet_queue_abort(PacketQueue* q);
    void packet_queue_start(PacketQueue* q);  // Resume queue operations
    int packet_queue_size(PacketQueue* q);
    void packet_queue_print_stats(PacketQueue* q, const char* name);  // Print statistics
    
    // helper functions
    void flushDecoders();
    
    // Clock functions
    void init_clock(Clock* c);
    void set_clock(Clock* c, double pts, double time);
    double get_clock(Clock* c);
    double get_master_clock();
    int get_master_sync_type();
    double compute_target_delay(double delay);
    
    // Video functions (封装视频初始化/清理)
    int video_init();       // 初始化视频解码器
    void video_close();     // 清理视频资源
    
    // Audio functions (封装音频初始化/清理)
    int audio_init();       // 初始化音频解码器
    void audio_close_decoder();  // 清理音频解码器资源
    int audio_open();       // 打开SDL音频设备
    void audio_close();     // 关闭SDL音频设备
    static void sdl_audio_callback(void* userdata, uint8_t* stream, int len);
    int audio_decode_frame(double* pts_ptr);
    int synchronize_audio(int nb_samples);
};

#endif // H_FFPLAYER_H
