# Hardware Decode Configuration Guide

## Overview

This project supports multiple hardware decoding methods and can automatically detect and select the best hardware decoder for your system.

## Supported Hardware Decode Modes

### 1. Auto Mode (Recommended)
```
decode_mode = 2  // HARDWARE_DECODE_AUTO
```
- System will automatically try all available hardware decoders by priority
- Windows priority: NVIDIA CUVID > Intel QSV > D3D11VA > DXVA2
- Falls back to software decode if no hardware decoder is available

### 2. Software Decode
```
decode_mode = 1  // SOFTWARE_DECODE
```
- Uses CPU for decoding (best compatibility, lower performance)

### 3. Intel QSV Hardware Decode
```
decode_mode = 3  // HARDWARE_DECODE_QSV
```
- Requires Intel integrated or discrete GPU
- Requires FFmpeg compiled with QSV support

### 4. NVIDIA CUVID Hardware Decode
```
decode_mode = 4  // HARDWARE_DECODE_CUVID
```
- Requires NVIDIA GPU
- Requires FFmpeg compiled with CUVID/CUDA support
- Best performance

### 5. D3D11VA Hardware Decode
```
decode_mode = 6  // HARDWARE_DECODE_D3D11VA
```
- Windows DirectX 11 Video Acceleration
- Supported by most modern Windows systems
- Good compatibility

### 6. DXVA2 Hardware Decode
```
decode_mode = 5  // HARDWARE_DECODE_DXVA2
```
- Windows DirectX Video Acceleration (legacy)
- Broader compatibility but lower performance than D3D11VA

## Configuration

Set `decode_mode` parameter in config file:

```ini
[video]
decode_mode = 2  # Use auto mode
```

Or in code:

```cpp
player->set_decode_mode(HARDWARE_DECODE_AUTO);
```

## Check FFmpeg Hardware Acceleration Support

Run the following command to see which hardware acceleration methods your FFmpeg supports:

```bash
ffmpeg -hwaccels
```

Example output:
```
Hardware acceleration methods:
dxva2
d3d11va
```

This means your FFmpeg supports DXVA2 and D3D11VA hardware decoding.

## Runtime Logs

The program will output decoder selection information during runtime:

### Successfully found hardware decoder:
```
Found hardware decoder: h264_d3d11va (D3D11VA)
codec_name: h264_d3d11va=>H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (D3D11VA acceleration)
```

### Fallback when hardware decoder is unavailable:
```
Hardware decoder not available: h264_cuvid (NVIDIA CUVID)
Hardware decoder not available: h264_qsv (Intel QSV)
Found hardware decoder: h264_d3d11va (D3D11VA)
```

### Complete fallback to software decode:
```
Hardware decoder not available: h264_cuvid (NVIDIA CUVID)
Hardware decoder not available: h264_qsv (Intel QSV)
Hardware decoder not available: h264_d3d11va (D3D11VA)
Hardware decoder not available: h264_dxva2 (DXVA2)
No hardware decoder found, will try software decode
Using software decoder: h264
```

## Performance Comparison

| Decode Method | Performance | CPU Usage | Compatibility |
|--------------|-------------|-----------|---------------|
| CUVID        | Fastest     | Very Low  | NVIDIA GPU required |
| QSV          | Fast        | Low       | Intel GPU required |
| D3D11VA      | Good        | Low       | Modern Windows |
| DXVA2        | Medium      | Low       | Most Windows |
| Software     | Slow        | High      | All systems |

## Troubleshooting

### Issue: Hardware decode fails
- Check if FFmpeg supports the hardware acceleration (use `ffmpeg -hwaccels`)
- Update GPU drivers to the latest version
- Try using auto mode (`HARDWARE_DECODE_AUTO`) to let the system choose

### Issue: Video artifacts
- Some hardware decoders may be incompatible with specific video formats
- Try switching to software decode mode

### Issue: Missing hardware acceleration support at compile time
If you need specific hardware acceleration support, add these options when compiling FFmpeg:
- NVIDIA CUVID: `--enable-cuda --enable-cuvid --enable-nvenc`
- Intel QSV: `--enable-libmfx`
- DXVA2/D3D11VA: Usually enabled by default on Windows

## Recommendations

1. **Use auto mode** (`HARDWARE_DECODE_AUTO`) to let the system automatically select the best hardware decoder
2. If you encounter compatibility issues, manually specify a specific hardware decode mode
3. For maximum compatibility, use software decode
4. For best performance on systems with NVIDIA GPU, use CUVID

