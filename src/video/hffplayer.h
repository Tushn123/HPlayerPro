#ifndef H_FFPLAYER_H
#define H_FFPLAYER_H

#include "HVideoPlayer.h"
#include "ffmpeg_util.h"

#include "hthread.h"
#include "hmutex.h"

#include <atomic>
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
    
    /* Usage examples:
     * player.seek(30000);              // Seek to 30 seconds (fast mode, to keyframe)
     * player.seek(30000, true);        // Seek to 30 seconds (accurate mode)
     * player.seekByPercent(50.0);      // Seek to 50% of video duration
     * player.seekRelative(5000);       // Skip forward 5 seconds
     * player.seekRelative(-5000);      // Skip backward 5 seconds
     * int64_t pos = player.getCurrentPosition(); // Get current position in ms
     * player.set_speed(2.0);           // 2x speed playback
     * player.set_speed(0.5);           // 0.5x speed (slow motion)
     */

private:
    virtual bool doPrepare();
    virtual void doTask();
    virtual bool doFinish();

    int open();
    int close();

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
    AVCodecContext*     codec_ctx;

    AVPacket* packet;
    AVFrame* frame;

    int video_stream_index;
    int audio_stream_index;
    int subtitle_stream_index;

    int video_time_base_num;
    int video_time_base_den;
    int audio_time_base_num;
    int audio_time_base_den;

    // for video scale
    AVPixelFormat   src_pix_fmt;
    AVPixelFormat   dst_pix_fmt;
    SwsContext*     sws_ctx;
    uint8_t*        data[4];
    int             linesize[4];
    HFrame          hframe;
    bool            sws_ctx_checked;  // Flag to check if sws_ctx format matches actual decoded frame

    // for audio
    AVCodecContext* audio_codec_ctx;
    AVFrame*        audio_frame;
    SwrContext*     swr_ctx;
    uint8_t*        audio_buffer;
    int             audio_buffer_size;
    int             audio_channels;
    int             audio_sample_rate;
    
    // SDL audio
    SDL_AudioDeviceID audio_dev_id;
    int             audio_hw_buf_size;
    std::queue<AudioFrame*> audio_frame_queue;
    hmutex_t        audio_queue_mutex;
    uint8_t*        audio_play_buf;
    int             audio_play_buf_size;
    int             audio_play_buf_index;
    
    // Clocks for A/V sync
    Clock           audio_clock;
    Clock           video_clock;
    int             av_sync_type;       // 0=audio master, 1=video master, 2=external
    double          audio_diff_cum;
    double          audio_diff_avg_coef;
    int             audio_diff_avg_count;
    double          frame_timer;
    double          frame_last_pts;
    double          frame_last_delay;

    // processing functions
    int processVideoPacket();
    int processAudioPacket();
    
    // helper functions
    void flushDecoders();
    
    // Clock functions
    void init_clock(Clock* c);
    void set_clock(Clock* c, double pts, double time);
    double get_clock(Clock* c);
    double get_master_clock();
    int get_master_sync_type();
    double compute_target_delay(double delay);
    
    // Audio functions
    int audio_open();
    void audio_close();
    static void sdl_audio_callback(void* userdata, uint8_t* stream, int len);
    int audio_decode_frame(double* pts_ptr);
    int synchronize_audio(int nb_samples);
    
    // Thread synchronization
    hmutex_t        decoder_mutex;       // Protects decoder operations
    hmutex_t        format_mutex;        // Protects fmt_ctx operations (read/seek)
    std::atomic<bool> is_seeking;        // Flag to indicate seeking in progress
};

#endif // H_FFPLAYER_H
