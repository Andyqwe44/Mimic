# Stream Protocol 使用示例

本目录演示如何用 `stream_protocol` 跨语言传输图像帧。

## 协议

每帧 20 字节头 + 像素数据（binary, little-endian）:

```
[magic:4 "FRAM"][w:4][h:4][ch:4][size:4][BGRA pixels: size bytes]
 magic = 0x4D415246   w,h = 尺寸   ch = 4   size = w*h*4
```

## 文件

| 文件 | 语言 | 角色 |
|------|------|------|
| `cpp_sender.hpp` | C++ | 发送端封装类（管道+TCP） |
| `cpp_pipe_send.cpp` | C++ | 示例: 捕获桌面 → stdout pipe |
| `rust_pipe_recv.rs` | Rust | 示例: stdout pipe → 接收帧 |
| `cpp_tcp_send.cpp` | C++ | 示例: 捕获桌面 → TCP :9999 |
| `python_tcp_recv.py` | Python | 示例: TCP → 接收帧 + 保存PNG |

## 运行

### 场景1: C++ pipe → Rust

```bash
# 终端1: 启动 C++ 发送端 (捕获桌面, 输出到stdout)
cd capture && build.cmd
./build/cpp_pipe_send.exe | tee raw_frames.bin

# 终端2: Rust 接收 (读取stdin, 打印帧信息)
cd examples
rustc rust_pipe_recv.rs -o rust_pipe_recv
./rust_pipe_recv < raw_frames.bin
```

### 场景2: C++ TCP → Python

```bash
# 终端1: 启动 C++ TCP 发送端 (捕获桌面, 监听 :9999)
./capture/build/cpp_tcp_send.exe

# 终端2: Python 接收 (连接 :9999, 显示帧)
python examples/python_tcp_recv.py
```

## 核心 API

C++ 发送一行搞定:
```cpp
TcpFrameSender sender;
sender.connect();                           // 127.0.0.1:9999
sender.send_frame(pixels, width, height);   // 自动构建协议头
```

Python 接收:
```python
with StreamClient() as client:              # 连接 127.0.0.1:9999
    for frame in client.frames():           # 迭代每一帧
        img = frame.to_numpy()              # ndarray (h, w, 4) BGRA
```
