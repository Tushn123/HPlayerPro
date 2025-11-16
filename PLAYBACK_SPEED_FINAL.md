# 倍速播放实现说明（最终版本）

## 🔍 为什么只修改时间戳不起作用？

### 问题发现

用户发现只修改 `playback_speed` 和帧时间戳**没有效果**。

### 根本原因

**HFFPlayer 的架构与 ffplay 根本不同**！

#### FFplay 架构（时间戳方案可行）
```
解码线程(不控速) → FrameQueue → video_refresh()
                                      ↓
                            检查 Clock::speed 和 pts
                            决定何时显示下一帧
```

**关键**：`video_refresh()` 会根据 `Clock::speed` 和帧 pts 计算延迟：
```c
// ffplay.c:1607
if (time < is->frame_timer + delay) {
    *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
    goto display;  // 时间未到，不显示
}
```

#### HFFPlayer 架构（时间戳方案不可行）
```
HThread::doTask() → setSleepPolicy(1000/fps) → 解码 → push_frame
                         ↓                              ↓
                    控制解码速度                   FrameQueue
                                                        ↓
UI Timer(固定频率) → onTimerUpdate() → pop_frame() → 立即显示
```

**问题**：`onTimerUpdate()` 不检查时间戳！

```cpp
// HVideoWidget.cpp:440-455
void HVideoWidget::onTimerUpdate() {
    if (pImpl_player->pop_frame(&videownd->last_frame) == 0) {
        videownd->update();  // 直接显示，不管时间戳！
    }
}
```

### 对比总结

| 特性 | FFplay | HFFPlayer |
|------|--------|-----------|
| 解码控制 | 不控速，持续解码 | `setSleepPolicy` 控制 |
| 显示控制 | 检查 Clock::speed + pts | 固定 Timer，不检查 pts |
| 速度实现 | 调整 Clock::speed | **必须调整睡眠时间** |
| 时间戳方案 | ✅ 可行 | ❌ 不可行 |

## ✅ 正确的实现方案

### 核心：调整解码线程睡眠时间

```cpp
void HFFPlayer::set_speed(double speed) {
    HVideoPlayer::set_speed(speed);
    
    // 关键：调整解码线程的睡眠时间
    if (fps > 0 && speed > 0.0) {
        int sleep_ms = (int)((1000.0 / fps) / speed);
        if (sleep_ms < 1) sleep_ms = 1;
        HThread::setSleepPolicy(HThread::SLEEP_UNTIL, sleep_ms);
    }
}
```

### 工作原理

```
正常速度 (1.0x):
解码 → 睡眠40ms → 解码 → 睡眠40ms → ... (25fps)
       ↓
UI Timer每20ms取一帧 → 正常播放

2倍速 (2.0x):
解码 → 睡眠20ms → 解码 → 睡眠20ms → ... (50fps)
       ↓
UI Timer每20ms取一帧 → 更快播放 ✅

0.5倍速 (0.5x):
解码 → 睡眠80ms → 解码 → 睡眠80ms → ... (12.5fps)
       ↓
UI Timer每20ms取一帧 → 更慢播放 ✅
```

## 📊 速度效果表

| 速度 | FPS | 睡眠时间 | 每秒解码帧数 | 效果 |
|------|-----|----------|-------------|------|
| 0.25x | 25 | 160ms | 6.25 帧/秒 | 极慢 |
| 0.5x | 25 | 80ms | 12.5 帧/秒 | 慢动作 |
| 1.0x | 25 | 40ms | 25 帧/秒 | 正常 |
| 1.5x | 25 | 27ms | 37.5 帧/秒 | 加速 |
| 2.0x | 25 | 20ms | 50 帧/秒 | 2倍速 |
| 4.0x | 25 | 10ms | 100 帧/秒 | 4倍速 |

**公式**：
```
睡眠时间(ms) = (1000 / fps) / speed
解码帧率 = fps * speed
```

## 🎯 完整实现

### 1. 头文件 (hffplayer.h)

```cpp
class HFFPlayer : public HVideoPlayer, public HThread {
public:
    // Playback speed control (overrides base class)
    virtual void set_speed(double speed) override;
};
```

### 2. 实现文件 (hffplayer.cpp)

```cpp
void HFFPlayer::set_speed(double speed) {
    HVideoPlayer::set_speed(speed);
    
    if (fps > 0 && speed > 0.0) {
        int sleep_ms = (int)((1000.0 / fps) / speed);
        if (sleep_ms < 1) sleep_ms = 1;
        HThread::setSleepPolicy(HThread::SLEEP_UNTIL, sleep_ms);
        hlogi("Playback speed set to %.2fx (decode sleep: %dms)", speed, sleep_ms);
    }
}

// 时间戳保持原样，不需要调整
int HFFPlayer::processVideoPacket() {
    // ...解码...
    if (video_time_base_num && video_time_base_den) {
        hframe.ts = frame->pts / (double)video_time_base_den * video_time_base_num * 1000;
    }
    push_frame(&hframe);
}
```

### 3. UI 层 (HVideoWidget)

```cpp
void HVideoWidget::setPlaybackSpeed(double speed) {
    if (pImpl_player) {
        pImpl_player->set_speed(speed);
        hlogi("Playback speed changed to %.2fx", speed);
    }
}
```

## 💻 使用示例

### 基本用法

```cpp
HVideoWidget widget;
widget.open(media);

// 2倍速播放
widget.setPlaybackSpeed(2.0);

// 慢动作
widget.setPlaybackSpeed(0.5);

// 恢复正常
widget.setPlaybackSpeed(1.0);
```

### 快捷键实现

```cpp
void HVideoWidget::keyPressEvent(QKeyEvent *e) {
    if (!pImpl_player) return;
    
    switch(e->key()) {
    case Qt::Key_BracketRight:  // ] 键加速
    {
        double speed = pImpl_player->get_speed();
        speed = qMin(speed + 0.25, 4.0);
        setPlaybackSpeed(speed);
        break;
    }
    case Qt::Key_BracketLeft:   // [ 键减速
    {
        double speed = pImpl_player->get_speed();
        speed = qMax(speed - 0.25, 0.25);
        setPlaybackSpeed(speed);
        break;
    }
    case Qt::Key_Backslash:     // \ 键恢复正常
        setPlaybackSpeed(1.0);
        break;
    }
}
```

### UI 控件

```cpp
// 添加速度选择下拉框到 toolbar
QComboBox* speedCombo = new QComboBox();
speedCombo->addItem("0.25x", 0.25);
speedCombo->addItem("0.5x", 0.5);
speedCombo->addItem("0.75x", 0.75);
speedCombo->addItem("1.0x (Normal)", 1.0);
speedCombo->addItem("1.25x", 1.25);
speedCombo->addItem("1.5x", 1.5);
speedCombo->addItem("2.0x", 2.0);
speedCombo->addItem("4.0x", 4.0);
speedCombo->setCurrentIndex(3);  // 默认 1.0x

connect(speedCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
    [this, speedCombo](int index) {
        double speed = speedCombo->itemData(index).toDouble();
        setPlaybackSpeed(speed);
    });

toolbar->layout()->addWidget(new QLabel("Speed:"));
toolbar->layout()->addWidget(speedCombo);
```

## 🔬 技术细节

### 线程睡眠策略

```cpp
// 在 open() 中初始化 (hffplayer.cpp:551)
HThread::setSleepPolicy(HThread::SLEEP_UNTIL, 1000 / fps);

// 在 set_speed() 中动态调整
HThread::setSleepPolicy(HThread::SLEEP_UNTIL, (1000 / fps) / speed);
```

`SLEEP_UNTIL` 模式确保帧之间的时间间隔准确。

### 边界条件

```cpp
// 1. 最小睡眠时间
if (sleep_ms < 1) sleep_ms = 1;  // 避免 CPU 100%

// 2. 速度范围限制（在基类中）
if (speed > 0.0 && speed <= 16.0) {
    playback_speed = speed;
}

// 3. fps 检查
if (fps > 0 && speed > 0.0) {
    // 计算睡眠时间
}
```

### Seek 后速度保持

```cpp
void HFFPlayer::seek(int64_t ms, bool accurate) {
    // ... seek 操作 ...
    // speed 保持不变，不需要重新设置
}
```

## ⚡ 性能分析

### CPU 使用率

| 速度 | 解码帧率 | 睡眠时间 | CPU 使用 | 说明 |
|------|---------|---------|---------|------|
| 0.25x | 6.25fps | 160ms | 很低 | 大量睡眠 |
| 0.5x | 12.5fps | 80ms | 低 | 较多睡眠 |
| 1.0x | 25fps | 40ms | 正常 | 标准 |
| 2.0x | 50fps | 20ms | 中等 | 解码加倍 |
| 4.0x | 100fps | 10ms | 较高 | 快速解码 |
| 8.0x | 200fps | 5ms | 很高 | 极快解码 |

### 帧队列状态

- **高速播放**：解码快，队列倾向于满 → 自然限速
- **低速播放**：解码慢，队列倾向于空 → 可能卡顿
- **正常播放**：队列平衡

### 优化建议

1. **限制最大速度**
   ```cpp
   // 根据 CPU 性能调整
   speed = qMin(speed, 4.0);
   ```

2. **监控帧队列**
   ```cpp
   FrameStats stats = pImpl_player->get_frame_stats();
   if (stats.cache_num < 3) {
       // 队列不足，可能需要降低速度
   }
   ```

3. **音频处理**
   - 当前：音频保持原速（可能不同步）
   - 改进：使用 `atempo` 滤镜同步音频速度

## 🎬 测试用例

```cpp
void testPlaybackSpeed() {
    HVideoWidget widget;
    widget.open(media);
    
    // 测试 1: 2倍速播放
    qDebug() << "Test 2x speed...";
    widget.setPlaybackSpeed(2.0);
    QTest::qWait(5000);  // 等待5秒，应该播放10秒内容
    
    // 测试 2: 慢动作
    qDebug() << "Test 0.5x speed...";
    widget.setPlaybackSpeed(0.5);
    QTest::qWait(5000);  // 等待5秒，应该播放2.5秒内容
    
    // 测试 3: 恢复正常
    qDebug() << "Test 1.0x speed...";
    widget.setPlaybackSpeed(1.0);
    QTest::qWait(5000);  // 等待5秒，应该播放5秒内容
    
    // 测试 4: Seek 后速度保持
    qDebug() << "Test speed after seek...";
    widget.setPlaybackSpeed(2.0);
    widget.pImpl_player->seek(30000);
    // 应该继续以2倍速播放
    QTest::qWait(2000);
    
    // 测试 5: 动态切换
    qDebug() << "Test dynamic switch...";
    widget.setPlaybackSpeed(4.0);
    QTest::qWait(1000);
    widget.setPlaybackSpeed(0.25);
    QTest::qWait(1000);
    widget.setPlaybackSpeed(1.0);
}
```

## 🔍 对比之前的错误方案

### 错误方案：只修改时间戳

```cpp
// ❌ 不起作用！
if (playback_speed != 1.0) {
    hframe.ts = base_ts + (accumulated_time / playback_speed);
}
```

**为什么不起作用**：
- UI Timer 固定频率取帧
- `pop_frame()` 不检查时间戳
- 解码速度未改变

### 正确方案：调整睡眠时间

```cpp
// ✅ 有效！
HThread::setSleepPolicy(SLEEP_UNTIL, (1000 / fps) / speed);
```

**为什么有效**：
- 直接控制解码速度
- UI Timer 自然地取到更快/更慢的帧
- 无需修改时间戳

## 📋 总结

### ✅ 关键点

1. **架构决定实现**：HFFPlayer 用睡眠控制帧率，必须调整睡眠时间
2. **简单有效**：只需一行代码 `setSleepPolicy`
3. **性能稳定**：解码速度变化可控
4. **无需时间戳调整**：保持原始时间戳即可

### 📝 与 FFplay 的差异

| 特性 | FFplay | HFFPlayer |
|------|--------|-----------|
| 速度控制 | Clock::speed | setSleepPolicy |
| 实现位置 | video_refresh | set_speed |
| 时间戳 | 需要 Clock 计算 | 保持原样 |
| 复杂度 | 高（精确同步） | 低（简单直接） |

### 🚀 优势

- **简单**：核心代码不到10行
- **有效**：直接控制解码速度
- **稳定**：性能可预测
- **易扩展**：可轻松添加更多功能

---

**实现完成** ✅  
基于 HFFPlayer 架构特点的正确倍速实现。

