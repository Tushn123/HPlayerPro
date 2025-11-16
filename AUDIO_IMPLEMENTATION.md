# SDL音频播放与音视频同步实现说明

## 实现概述

本实现为HFFPlayer添加了SDL音频播放功能和音视频同步机制，参考了ffplay.c的实现方式。

## 主要功能

### 1. Clock机制（音视频同步）
- **结构**: `Clock` - 用于跟踪音频和视频的时间戳
- **函数**:
  - `init_clock()` - 初始化时钟
  - `set_clock()` - 设置时钟PTS
  - `get_clock()` - 获取当前时钟值
  - `get_master_clock()` - 获取主时钟（有音频时为音频时钟，无音频时为视频时钟）

### 2. 音频帧队列
- **结构**: `AudioFrame` - 存储解码后的音频数据和PTS
- **队列**: `std::queue<AudioFrame*> audio_frame_queue` - FIFO队列
- **线程安全**: 使用`audio_queue_mutex`保护队列操作

### 3. SDL音频播放
- **初始化**: `audio_open()` - 打开SDL音频设备
  - 配置采样率、通道数、格式等
  - 设置回调函数
- **回调**: `sdl_audio_callback()` - SDL音频回调，从队列获取数据并播放
- **关闭**: `audio_close()` - 关闭SDL音频设备并清理资源

### 4. 音频解码
- **函数**: `processAudioPacket()` - 解码音频包并放入队列
  - 使用SwrContext重采样为PCM S16LE格式
  - 计算音频PTS
  - 创建AudioFrame并加入队列

### 5. 音视频同步
- **视频同步**: 在`processVideoPacket()`中
  - 更新视频时钟
  - 与音频时钟比较
  - 如果视频超前，延迟显示
- **音频同步**: 在SDL回调中
  - 从队列获取音频帧
  - 更新音频时钟
  - 持续提供音频数据

### 6. 无音频流处理
- **检测**: 在`open()`函数中检测是否存在音频流
- **降级**: 无音频时自动使用视频时钟作为主时钟
- **日志**: 记录"No audio stream"信息

## 关键代码位置

### 头文件 (hffplayer.h)
- **第15-22行**: Clock结构定义
- **第25-37行**: AudioFrame结构定义
- **第131-145行**: SDL音频相关成员变量
- **第154-165行**: Clock和音频函数声明

### 实现文件 (hffplayer.cpp)

#### 初始化
- **第131-145行**: 构造函数中初始化SDL音频变量和Clock

#### 音频打开/关闭
- **第562-567行**: open()中调用audio_open()
- **第577-579行**: 检查是否有音频流
- **第587行**: close()中调用audio_close()
- **第1243-1280行**: audio_open()实现
- **第1282-1303行**: audio_close()实现

#### 音频处理
- **第1027-1101行**: processAudioPacket()将解码的音频放入队列
- **第1199-1238行**: audio_decode_frame()从队列获取音频帧

#### Clock实现
- **第1160-1191行**: Clock相关函数实现

#### SDL回调
- **第1193-1235行**: sdl_audio_callback()实现

#### 音视频同步
- **第1003-1020行**: processVideoPacket()中的音视频同步逻辑

#### Seek支持
- **第702-709行**: seek()中清空音频队列
- **第670-671行**: flushDecoders()中重置Clock

## 使用说明

### 编译要求
- FFmpeg库（libavcodec, libavformat, libavutil, libswresample）
- SDL2库

### 运行时行为
1. **有音频流**: 使用音频作为主时钟，视频同步到音频
2. **无音频流**: 使用视频时钟，正常播放
3. **音频初始化失败**: 降级为仅视频播放

### 调试信息
- "Audio decoder initialized" - 音频解码器初始化成功
- "SDL audio opened successfully" - SDL音频设备打开成功
- "No audio stream or audio initialization failed" - 无音频或初始化失败

## 注意事项

### 线程安全
- 音频队列操作使用mutex保护
- SDL回调在独立线程中运行
- Seek时需要清空队列并重置时钟

### 内存管理
- AudioFrame使用RAII模式，析构时自动释放内存
- 音频缓冲区使用av_malloc/av_free管理

### 性能考虑
- 队列最大15MB，防止内存溢出
- 音视频同步阈值10ms，平衡流畅度和同步精度
- 最大延迟500ms，防止过度延迟

## 已知限制

1. **简化的同步**: 当前实现是简化版本，没有实现音频速率调整
2. **Linter警告**: av_gettime_relative在linter中报未定义，但编译时正常
3. **缓冲管理**: 固定缓冲区大小，可能需要根据实际情况调整

## 测试建议

1. 测试有音频的视频文件
2. 测试无音频的视频文件
3. 测试seek操作
4. 测试播放速度调整
5. 测试长时间播放的稳定性

