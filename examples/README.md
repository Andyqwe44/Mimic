# Stream Protocol 使用示例

本目录演示如何用 `stream_protocol` 跨语言传输图像帧。

## 协议

### 新协议 (protocol/) — 12 字节头

```
[magic:4][body_size:4][type_tag:4][body: body_size bytes]
 magic = 0x4D415246   type_tag = 1(BGRA)  body = [w:4][h:4][ch:4][reserved:4][pixels]
```

### 旧协议 (stream_protocol) — 8 字节头

```
[magic:4][size:4][pixels]
```

## WGC Benchmark (新增)

### 架构

```
┌─ wgc_bench_send.exe (C++) ──────┐    TCP :9999    ┌─ wgc_bench_recv.exe (Rust) ─┐
│  WGC FramePool 或 DXGI 捕获      │ ──────────────→ │  接收帧 → 统计 FPS → 存盘  │
│  每阶段微秒级时间戳               │                 │  每秒报告 FPS + 带宽        │
└─────────────────────────────────┘                 └────────────────────────────┘
```

### 构建

```bash
cd capture && build.cmd    # 构建 capture_wgc.exe + benchmark 工具
```

### 运行

```bash
# 终端1: 启动接收端
cd examples/build
./wgc_bench_recv.exe --port 9999 --save-every 60 --out-dir frames

# 终端2: 启动发送端 (窗口捕获)
./bench_send.exe <hwnd> --port 9999 --scale 1280 --no-wait

# 或桌面捕获 (DXGI, 60Hz)
./bench_send.exe 0 --port 9999 --scale 1280 --no-wait
```

### 参数说明

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `hwnd` | (必需) | 窗口句柄, 0 = 桌面(DXGI) |
| `--port N` | 9999 | TCP 端口 |
| `--scale N` | 1280 | 最大尺寸, 0 = 不缩放 |
| `--no-wait` | false | 帧间不休眠, 最大 FPS |
| `--save-every N` | 60 | 每 N 帧存盘, 0 = 不存 |

### 独立 WGC 工具

```bash
# 单帧捕获 (输出到 stdout)
capture_wgc.exe <hwnd> --single --scale 1280 > frame.bgra

# 流模式 (stdin 输入 'q' 退出)
capture_wgc.exe <hwnd> --stream --scale 1280
```

## 性能分析

### Timing 分解 (每帧)

单帧 WGC 捕获 (1800×1020 → 1280×725):

| 阶段 | 耗时 | 占比 |
|------|------|------|
| TryGetNextFrame | 5μs | 0.1% |
| CopyResource (GPU→staging) | 861μs | 11.2% |
| Map + memcpy (CPU readback) | 6,833μs | 88.7% |
| **总计** | **7,699μs** | **100%** |

DXGI 桌面捕获 (1536×864 → 1280×720):

| 阶段 | 耗时 | 占比 |
|------|------|------|
| AcquireNextFrame + readback | 7,233μs | 67.8% |
| Pack (BGRA header) | 1,967μs | 18.4% |
| TCP send | 1,468μs | 13.8% |
| **总计** | **10,669μs** | **100%** |

### 瓶颈分析

1. **CPU readback (Map+memcpy)**: 占总时间 89%，主要瓶颈
   - 原因: GPU→CPU staging texture 需要 PCIe 传输 + 逐行 memcpy
   - 1800×1020×4 = 7.37MB, readback 6.8ms = 1.08 GB/s
   - 优化方向: 共享 GPU 内存、direct GPU→network (RDMA)、缩小分辨率

2. **WGC 帧率受窗口更新率限制**: 静态窗口 ~2 FPS, 动态窗口可达 60+ FPS
   - WGC 仅在窗口内容变化时产生新帧
   - 如需恒 60 FPS: 用 DXGI 桌面复制 (hwnd=0)

3. **DXGI 桌面帧率受显示器刷新率限制**: 
   - 60Hz 显示器最大 60 FPS
   - 0ms 超时非阻塞轮询可达到刷新率上限
   - 阻塞式超时 (16ms+) 会丢帧降低实际 FPS

### 优化路径 → 60+ FPS

| 优化 | 预期提升 | 实现难度 |
|------|----------|----------|
| **Scale down** (1280→960→640) | readback 减半~75% | ✅ 已实现 |
| **非阻塞轮询** (timeout=0) | 消除无效等待 | ✅ 已实现 |
| **TCP_NODELAY + 大缓冲** | send 延迟↓ | ✅ 已实现 |
| **三重缓冲 staging** | GPU/CPU 流水线重叠 | ✅ 已实现 |
| **分离采集/发送线程** | 采集不阻塞于 TCP | 中 |
| **Dirty rect / 帧差** | 仅发送变化区域 | 中 |
| **GPU 共享纹理** | 零拷贝到消费端 | 高 |
| **GPU H.264 编码** | 带宽降至 1/100 | 高 (MF encoder 有 bug) |

### 理论最大 FPS

```
WGC 窗口:  min(window_update_rate, 1000/4.6ms) = min(60, 217) = 60 FPS (动态窗口)
DXGI 桌面: min(display_refresh, 1000/10.7ms)  = min(60, 93)   = 60 FPS (60Hz 显示器)
```

## 文件

| 文件 | 语言 | 角色 |
|------|------|------|
| `wgc_bench_send.cpp` | C++ | WGC/DXGI 采集 → TCP 发送 (含时间戳) |
| `wgc_bench_recv.rs` | Rust | TCP 接收 → 统计 FPS → 存盘 |
| `../capture/src/capture_wgc.cpp` | C++ | WGC FramePool 采集库 |
| `../capture/src/capture_wgc_main.cpp` | C++ | 独立 WGC CLI 工具 |
| `../capture/include/capture_wgc.hpp` | C++ | WGC 采集头文件 |
| `run_bench.bat` | Batch | 一键运行 benchmark |

### 旧示例 (stream_protocol)

| 文件 | 语言 | 角色 |
|------|------|------|
| `cpp_sender.hpp` | C++ | 发送端封装类（管道+TCP） |
| `cpp_pipe_send.cpp` | C++ | 示例: 捕获桌面 → stdout pipe |
| `rust_pipe_recv.rs` | Rust | 示例: stdout pipe → 接收帧 |
| `cpp_tcp_send.cpp` | C++ | 示例: 捕获桌面 → TCP :9999 |
| `python_tcp_recv.py` | Python | 示例: TCP → 接收帧 + 保存PNG |
