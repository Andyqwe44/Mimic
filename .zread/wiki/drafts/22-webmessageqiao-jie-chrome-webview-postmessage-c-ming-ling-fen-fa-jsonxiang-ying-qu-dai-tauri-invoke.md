在 `monitor_app` 中，前端 React 应用与 C++ 后端之间的通信不依赖 Tauri 的 `invoke` API，而是直接使用 WebView2 原生提供的 `chrome.webview.postMessage` 机制。这是一个"纯 C++ WebView2 宿主"架构的核心设计决策：剥离了 Rust/Tauri 中间层，由 C++ 原生代码直接处理前端发来的每一个命令，返回 JSON 响应。

本文将解剖这一桥接的完整工作流：从 React 侧的 `hostCall()` 封装，到 C++ 侧的 `HandleWebMessage` → `dispatch_command` → `PostJsonToWebView` 链，再到 JSON 命令的注册与分发模式。

Sources: [main.cpp](monitor_app/src/main.cpp#L217-L247), [commands.h](monitor_app/src/commands.h), [commands.cpp](monitor_app/src/commands.cpp#L535-L595), [App.tsx](monitor_web/src/App.tsx#L1-L53)

---

## 整体架构：WebMessage 桥接的三层模型

```
┌─────────────────────────────────────────────────────────────────────┐
│  React Frontend (monitor_web/)                                      │
│                                                                     │
│  hostCall(cmd, args) {                                             │
│    chrome.webview.postMessage(JSON.stringify({cmd, id, args}))     │
│    return new Promise((resolve, reject) => {                       │
│      _pending.set(id, {resolve, reject, timer})                    │
│    })                                                              │
│  }                                                                 │
│                                                                     │
│  chrome.webview.addEventListener('message', (e) => {               │
│    const msg = JSON.parse(e.data)                                   │
│    _pending.get(msg.id).resolve(msg.result)   ◄── 异步回调完成      │
│  })                                                                 │
└──────────────────────┬──────────────────────────────────────────────┘
                       │ chrome.webview.postMessage()
                       ▼
┌─────────────────────────────────────────────────────────────────────┐
│  C++ Host (monitor_app/) — WebView2 原生通信                        │
│                                                                     │
│  add_WebMessageReceived(handler)   ◄── 注册消息监听                  │
│       │                                                             │
│       ▼                                                             │
│  HandleWebMessage(wstring msg) {                                   │
│    json = WideCharToMultiByte(CP_UTF8, ...)                        │
│    result = dispatch_command(json)   ◄── 命令分发                    │
│    PostJsonToWebView(result)          ◄── 响应返回                   │
│  }                                                                 │
│                                                                     │
│  PostJsonToWebView(string json) {                                  │
│    g_webview->PostWebMessageAsJson(wstring)                        │
│  }                                                                 │
└──────────────────────┬──────────────────────────────────────────────┘
                       │ dispatch_command(json)
                       ▼
┌─────────────────────────────────────────────────────────────────────┐
│  命令注册表 (commands.cpp)                                          │
│                                                                     │
│  dispatch_command(json) {                                          │
│    cmd = json_get_str(json, "cmd")     ◄── 解析命令名               │
│    id  = json_get_int(json, "id")      ◄── 请求ID（关联 Promise）   │
│    args = json_get_obj(json, "args")   ◄── 参数对象                 │
│                                                                     │
│    if (cmd == "list_windows")        result = cmd_list_windows()    │
│    if (cmd == "capture_window")      result = cmd_capture_window()  │
│    if (cmd == "capture_stream_start") result = cmd_stream_start()   │
│    if (cmd == "read_logs")           result = cmd_read_logs()       │
│    ... 总计 12+ 个命令分支                                           │
│                                                                     │
│    return '{"id":' + id + ',"result":' + result + '}'              │
│  }                                                                  │
└─────────────────────────────────────────────────────────────────────┘
```

这个三层模型替代了 Tauri 的完整 invoke → Rust handler → Tauri Command 体系。C++ 直接承担了 Tauri 中 Rust 角色的全部职责：消息路由、参数解析、业务逻辑、响应序列化。

Sources: [main.cpp](monitor_app/src/main.cpp#L217-L237), [commands.cpp](monitor_app/src/commands.cpp#L535-L595), [App.tsx](monitor_web/src/App.tsx#L1-L53)

---

## React 侧：hostCall() — 取代 Tauri invoke 的 Promise 封装

### 函数签名与调用方式

前端不再导入 `@tauri-apps/api` 的 `invoke`，而是使用本地定义的 `hostCall` 函数：

```typescript
// monitor_web/src/App.tsx — 核心通信函数
function hostCall(cmd: string, args?: Record<string, any>): Promise<any> {
  return new Promise((resolve, reject) => {
    const id = ++_callId;
    const timer = setTimeout(() => {
      _pending.delete(id);
      reject(new Error(`hostCall timeout: ${cmd}`));
    }, 30000);                          // 30s 超时保护
    _pending.set(id, { resolve, reject, timer });
    (window as any).chrome.webview.postMessage(
      JSON.stringify({ cmd, id, args: args || {} })
    );
  });
}
```

### 调用示例

| 前端调用 | 生成的 JSON 载荷 | 对应 C++ 命令 |
|----------|-----------------|--------------|
| `hostCall('list_windows')` | `{"cmd":"list_windows","id":1,"args":{}}` | `cmd_list_windows()` |
| `hostCall('capture_window', {hwnd: 0x1234, method: 'WGC'})` | `{"cmd":"capture_window","id":2,"args":{"hwnd":4660,"method":"WGC"}}` | `cmd_capture_window(...)` |
| `hostCall('capture_stream_start', {hwnd, method, transport})` | `{"cmd":"capture_stream_start","id":3,"args":{...}}` | `cmd_capture_stream_start(...)` |

### Promise 映射机制

- **`_callId`**：全局递增整数，确保每个请求有唯一 ID
- **`_pending` Map**：以 `id` 为键，存储 `{resolve, reject, timer}` 三元组
- **超时处理**：30 秒未收到响应自动 reject，防止内存泄漏
- **消息监听**：通过 `chrome.webview.addEventListener('message', ...)` 接收 C++ 返回的 JSON，解析出 `id` 和 `result`，找到对应的 Promise resolve

这个设计模式与 Tauri 的 invoke 在语义上完全等价——调用方得到一个 Promise，异步等待结果返回——但底层通信不再是 Tauri 的 IPC bridge，而是 WebView2 的原生 WebMessage 通道。

Sources: [App.tsx](monitor_web/src/App.tsx#L1-L53)

---

## C++ 侧：HandleWebMessage → dispatch_command 的命令分发链

### 消息接收：`HandleWebMessage`

```cpp
// monitor_app/src/main.cpp
void HandleWebMessage(const std::wstring& msg)
{
    // WideChar → UTF-8
    int len = WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string json(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1, &json[0], len, nullptr, nullptr);

    // 命令分发
    std::string result = dispatch_command(json);

    // 发送响应回 WebView
    if (!result.empty()) {
        PostJsonToWebView(result);
    }
}
```

关键设计要点：

1. **字符编码转换**：WebView2 的 `TryGetWebMessageAsString` 返回 `LPWSTR`（UTF-16），C++ 后端使用 UTF-8 内部编码，因此需要 `WideCharToMultiByte` 转换
2. **无阻塞保证**：`HandleWebMessage` 在 WebView2 的 UI 线程上调用，因此每个命令处理应尽可能轻量；流式捕获等长时间操作在独立线程中运行
3. **空响应过滤**：如果 `dispatch_command` 返回空字符串（如 fire-and-forget 模式），则跳过 `PostJsonToWebView`，避免前端收到无效消息

### 命令分发器：`dispatch_command`

```cpp
// monitor_app/src/commands.cpp — 命令注册表的枢纽
std::string dispatch_command(const std::string& json) {
    std::string cmd = json_get_str(json, "cmd");
    int id = json_get_int(json, "id");
    std::string args = json_get_obj(json, "args");

    std::string result;
    if (cmd == "list_windows") result = cmd_list_windows();
    else if (cmd == "list_processes") result = cmd_list_processes();
    else if (cmd == "capture_window")
        result = cmd_capture_window(json_get_uint64(args, "hwnd"),
                                   json_get_str(args, "method"));
    else if (cmd == "capture_stream_start")
        result = cmd_capture_stream_start(json_get_uint64(args, "hwnd"),
                                         json_get_str(args, "method"),
                                         json_get_str(args, "transport"));
    else if (cmd == "capture_stream_stop") result = cmd_capture_stream_stop();
    else if (cmd == "read_logs") result = cmd_read_logs(json_get_int(args, "max_files"));
    else if (cmd == "clear_log") result = cmd_clear_log();
    else if (cmd == "log_ui_event")
        result = cmd_log_ui_event(json_get_str(args, "event"),
                                  json_get_str(args, "detail"));
    else if (cmd == "benchmark_methods")
        result = cmd_benchmark_methods(json_get_uint64(args, "hwnd"),
                                      json_get_str(args, "method"));
    else if (cmd == "debug_dump_frames") result = cmd_debug_dump_frames(json_get_int(args, "enable") != 0);
    else if (cmd == "highlight_window") result = cmd_highlight_window(json_get_uint64(args, "hwnd"));
    else if (cmd == "screen_info") { /* ... 屏幕尺寸 ... */ }
    else if (cmd == "window_state") { /* ... 窗口状态查询 ... */ }
    else if (cmd == "stream_poll") { /* ... 轮询最新帧 ... */ }

    if (id <= 0) return result;   // fire-and-forget: 无 id 字段
    if (result.empty()) return "{\"id\":" + std::to_string(id) + ",\"error\":\"unknown command\"}";
    return "{\"id\":" + std::to_string(id) + ",\"result\":" + result + "}";
}
```

### 命令注册表的完整清单

| 命令名 | 功能 | 参数 | 返回 |
|--------|------|------|------|
| `list_windows` | 枚举可见窗口 | 无 | `WindowInfo[]` JSON 数组 |
| `list_processes` | 枚举运行进程 | 无 | `ProcessInfo[]` JSON 数组 |
| `capture_window` | 单帧截图（所有方法） | `hwnd`, `method` | base64 PNG + 窗口坐标 + 方法名 |
| `capture_stream_start` | 启动实时流 | `hwnd`, `method`, `transport` | `{"ok":true}` |
| `capture_stream_stop` | 停止实时流 | 无 | `{"ok":true}` |
| `read_logs` | 读取日志缓冲区 | `max_files` | `{"live":"...","files":[...]}` |
| `clear_log` | 清空日志 | 无 | `{"ok":true}` |
| `log_ui_event` | 记录 UI 交互日志 | `event`, `detail` | `{"ok":true}` |
| `benchmark_methods` | 基准测试所有捕获方法 | `hwnd`, `method` | `{"results":[...]}` |
| `debug_dump_frames` | 开关帧转储 | `enable` (0/1) | `{"ok":true}` |
| `highlight_window` | 黄色边框高亮窗口 | `hwnd` | `{"ok":true}` 或 `{"ok":false}` |
| `screen_info` | 获取屏幕尺寸 | 无 | `{"w":1920,"h":1080}` |
| `window_state` | 查询窗口状态 | `hwnd` | `"foreground"` / `"minimized"` 等 |
| `stream_poll` | 轮询最新帧（Canvas 回退） | 无 | base64 帧数据 |

Sources: [commands.cpp](monitor_app/src/commands.cpp#L535-L595), [commands.cpp](monitor_app/src/commands.cpp#L1-L18)

---

## JSON 工具层：轻量级参数解析

项目刻意避免引入 JSON 解析库（如 nlohmann/json），而是手写了一套仅覆盖命令通信所需子集的工具函数：

```cpp
// monitor_app/src/json_helper.h
inline uint64_t json_get_uint64(const std::string& json, const std::string& key) {
    std::string s = "\"" + key + "\":";
    size_t p = json.find(s);
    if (p == std::string::npos) return 0;
    p += s.length();
    return strtoull(json.c_str() + p, nullptr, 10);
}

inline std::string json_get_str(const std::string& json, const std::string& key) {
    std::string s = "\"" + key + "\":\"";
    size_t p = json.find(s);
    if (p == std::string::npos) return "";
    p += s.length();
    size_t e = json.find('"', p);
    if (e == std::string::npos) return "";
    return json.substr(p, e - p);
}
```

### 设计权衡

| 方案 | 优点 | 缺点 |
|------|------|------|
| **手写 JSON 解析**（本项目） | 零依赖，编译快，二进制体积小 | 不支持嵌套/转义/Unicode 完整规范 |
| **nlohmann/json** | 功能完整，生态成熟 | 增加约 30% 编译时间和依赖管理 |
| **RapidJSON** | 高性能 SAX/DOM 双模式 | 学习曲线高，头文件体积大 |

本项目之所以选择手写 JSON 解析，是因为 WebMessage 通信格式是"受控的"——前端序列化的 JSON 结构是确定的，不会出现嵌套对象或特殊字符转义。`dispatch_command` 只读取顶层的 `cmd`、`id`、`args` 三个字段，而 `args` 中的数值参数也仅限于 `hwnd`（`uint64`）、`method`（字符串）、`max_files`（整数）等简单类型。这种有限子集让手写解析既安全又高效。

Sources: [json_helper.h](monitor_app/src/json_helper.h#L1-L51)

---

## 与 Tauri invoke 的对比分析

### 架构层面的核心差异

```
Tauri 方案                                 本项目方案
─────────                                  ─────────
React App                                  React App
    │ invoke()                                  │ chrome.webview.postMessage()
    ▼                                           ▼
Tauri IPC Bridge (Rust)                     WebView2 Host (C++)
    │ Tauri Command                             │ dispatch_command()
    ▼                                           ▼
Rust handler   →  FFI → C++ lib            C++ handler (直接)
    │ return                                       │ return
    ▼                                               ▼
JSON → Tauri → React                         JSON → PostWebMessageAsJson → React
```

### 优劣对比

| 维度 | Tauri invoke | 本项目 WebMessage 桥接 |
|------|-------------|----------------------|
| **中间层** | Rust IPC 桥接（必须过 Tauri 核心） | 无中间层，C++ 直接响应 WebView2 事件 |
| **延迟** | 每次调用经历 Rust 序列化/反序列化 + FFI 边界 | 一次 `WideCharToMultiByte` + 命令函数调用 |
| **依赖** | 需要 Rust 工具链 + Tauri SDK | 零额外依赖，仅 WebView2 运行时 |
| **类型安全** | Rust 侧有类型检查（Tauri Command 参数校验） | 运行时 JSON 解析，无编译期类型检查 |
| **前端 API** | `import { invoke } from '@tauri-apps/api'` | `(window as any).chrome.webview.postMessage()` |
| **流式数据** | 需额外通道（如 Tauri events + listen） | 原生支持 SharedBuffer 零拷贝推送 |
| **调试** | Chrome DevTools 可以看到 Tauri 内部 IPC | DevTools 中直接看到 `postMessage` / `message` 事件 |
| **学习曲线** | 需要 Rust + Tauri 框架知识 | 仅需 WebView2 API + C++ 基础 |

### 为什么本项目选择取消 Tauri

核心原因是**架构简化**。本项目已有四个 C++ 模块（capture/input/game/agent），如果在监控面板上再加一层 Rust/Tauri，则：

1. **编译链加倍**：需要安装 Rust、cargo、Tauri CLI，与已有 MSVC 工具链交叉维护
2. **调试复杂度**：Bug 可能出现在 C++ 层、FFI 绑定层、Tauri IPC 层三个不同位置
3. **二进制体积**：Tauri 的 WebView2 启动器 + Rust 运行时默认增加约 5-10MB

直接使用 WebView2 的 C++ API 订阅 WebMessage 事件，将 C++ 层同时作为 "后端服务" 和 "IPC 路由"——一个进程完成所有工作。

Sources: [main.cpp](monitor_app/src/main.cpp#L1-L30), [main.cpp](monitor_app/src/main.cpp#L217-L247)

---

## 扩展新命令的模式

添加一个新命令（例如 `get_performance_stats`）只需三步：

### 步骤 1：在 `commands.cpp` 中实现处理函数

```cpp
static std::string cmd_get_performance_stats() {
    // 收集性能数据
    MEMORYSTATUSEX mem = {sizeof(MEMORYSTATUSEX)};
    GlobalMemoryStatusEx(&mem);
    char buf[256];
    snprintf(buf, sizeof(buf),
        R"({"mem_usage_pct":%llu,"cpu_count":%lu})",
        mem.dwMemoryLoad, (unsigned long)std::thread::hardware_concurrency());
    return buf;
}
```

### 步骤 2：在 `dispatch_command` 添加分支

```cpp
else if (cmd == "get_performance_stats") result = cmd_get_performance_stats();
```

### 步骤 3：前端调用

```typescript
const stats = await hostCall('get_performance_stats');
// stats → { mem_usage_pct: 45, cpu_count: 8 }
```

这个扩展模式比 Tauri 的 Command 定义（需要新建 Rust struct、实现 Serialize/Deserialize、注册到 invoke_handler）更直接。对于不需要类型安全的项目内部命令来说，这种"简单即正确"的方式减少了样板代码。

Sources: [commands.cpp](monitor_app/src/commands.cpp#L535-L595)

---

## 响应路径：PostJsonToWebView 与 Promise 完成

C++ 端完成命令处理后，通过 `PostJsonToWebView` 将结果发回前端：

```cpp
void PostJsonToWebView(const std::string& json)
{
    if (g_webview) {
        int len = MultiByteToWideChar(CP_UTF8, 0, json.c_str(), -1, nullptr, 0);
        std::wstring w(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, json.c_str(), -1, &w[0], len);
        g_webview->PostWebMessageAsJson(w.c_str());
    }
}
```

前端收到消息后，通过事件监听器解析并完成 Promise：

```typescript
chrome.webview.addEventListener('message', (e: any) => {
    const msg = JSON.parse(e.data);
    const pending = _pending.get(msg.id);
    if (pending) {
        clearTimeout(pending.timer);
        _pending.delete(msg.id);
        pending.resolve(msg.result);
    }
});
```

这个方案的关键特性：

- **JSON 嵌入 ID**：响应 JSON 中包含原始请求的 `id` 字段，前端据此匹配到对应的 Promise
- **无状态通信**：C++ 端不维护任何会话状态，每次请求独立解析、独立响应
- **超时自愈**：前端 30 秒超时自动 reject Promise，防止 C++ 端异常导致前端死锁

Sources: [main.cpp](monitor_app/src/main.cpp#L237-L247), [App.tsx](monitor_web/src/App.tsx#L1-L53)

---

## 与 SharedBuffer 零拷贝通道的关系

需要特别说明的是，WebMessage 桥接和 `SharedBuffer` 通道是两个**独立但互补**的通信机制：

| 通道 | 方向 | 数据量 | 用途 |
|------|------|--------|------|
| **WebMessage (JSON)** | 双向 | 小（< 10KB） | 命令请求/响应、状态查询、UI 交互 |
| **SharedBuffer** | C++ → 前端 | 大（帧数据 4MB+） | 实时帧推送，零拷贝 Canvas 渲染 |

`SharedBuffer` 通道使用 `ICoreWebView2_17::PostSharedBufferToScript` 将 GPU 帧缓冲区直接推送给前端的 `ArrayBuffer`，绕过 JSON 序列化和 base64 编码的巨大开销。这是流式预览的主路径——当 SharedBuffer 可用时，实时帧通过此通道以 >30 FPS 渲染到 Canvas，而 WebMessage 通道仅用于控制命令和状态同步。

如果 SharedBuffer 不可用（如旧版 WebView2 运行时），流式预览回退到 MJPEG HTTP 流（通过 `mjpeg_server.cpp` 推送 JPEG 帧到 `<img>` 标签），而 WebMessage 桥接依然保持控制通道的连通性。

Sources: [main.cpp](monitor_app/src/main.cpp#L200-L215), [App.tsx](monitor_web/src/App.tsx#L500-L540)

---

## 下一步阅读

- **流式传输管线详解**：[流式传输管线：WGC→GPU拷贝→SharedBuffer直推Canvas（主路径）/ WIC JPEG编码→MJPEG HTTP多部分传输（回退）](23-liu-shi-chuan-shu-guan-xian-wgc-gpukao-bei-sharedbufferzhi-tui-canvas-zhu-lu-jing-wic-jpegbian-ma-mjpeg-httpduo-bu-fen-chuan-shu-hui-tui) — 深入了解 SharedBuffer 零拷贝通道和 MJPEG 回退机制
- **监控面板功能全览**：[监控面板功能：仪表盘/窗口捕获预览/FPS计数/日志环缓冲区/窗口选择器/设置页面](24-jian-kong-mian-ban-gong-neng-yi-biao-pan-chuang-kou-bu-huo-yu-lan-fpsji-shu-ri-zhi-huan-huan-chong-qu-chuang-kou-xuan-ze-qi-she-zhi-ye-mian) — 查看所有 UI 功能如何调用这些命令
- **纯 C++ WebView2 宿主架构**：[纯C++ WebView2宿主：Win32窗口 + React UI（与Tauri解耦，同一份前端代码100%不变）](21-chun-c-webview2su-zhu-win32chuang-kou-react-ui-yu-taurijie-ou-tong-fen-qian-duan-dai-ma-100-bu-bian) — WebMessage 桥接的基础设施