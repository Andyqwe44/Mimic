# 用户输入转发 — 完整计划书

## 背景

后期 Python 视觉 AI 模型通过 TCP 发来动作指令 → C++ 模拟用户输入操控目标窗口。
现在需要先做**前端手动输入调试层**：用户在 GUI 中看到目标窗口的实时截图，
通过鼠标和键盘直接在预览画面上操作，C++ 将操作转发到目标窗口。

当前状态：只实现了单击转发（`MonitorView` 点击 → `cmd_send_input` click）。

---

## 一、输入模拟层级总览

| 层级 | 实现方式 | 原理 | 兼容性 | UIPI 绕过 | 反作弊检测 |
|------|---------|------|--------|-----------|-----------|
| **应用层** | `SendInput` API | 向系统输入队列注入事件，与真实硬件走相同路径 | ★★★★★ 绝大多数应用 | ❌ 不能向高权限窗口发 | 可能被检测 |
| **窗口消息层** | `PostMessage` / `SendMessage` | 直接向目标窗口队列投递 `WM_*` 消息 | ★★★☆☆ 部分应用无响应 | 部分绕过 | 较难检测 |
| **UI自动化层** | Windows Automation API (`IUIAutomation`) | COM 接口操作 UI 元素（按钮、输入框等），语义化 | ★★☆☆☆ 仅支持 UIA Provider 控件 | 受限 | N/A |
| **驱动层** | Interception / 虚拟 HID 驱动 | 内核级虚拟输入设备，系统视为真实硬件 | ★★★★☆ 依赖驱动安装 | ✅ 完全绕过 | 极难检测 |

### 各层级详细分析

#### 1. 应用层 — `SendInput`（推荐主力）

```
SendInput() → 系统输入队列 → 原始输入线程(RIT) → 消息队列 → 窗口过程
                 ↑ 与真实鼠标/键盘走完全相同路径
```

**鼠标能力：**
- 绝对移动：`MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE`（虚拟桌面 0-65535 坐标）
- 相对移动：`MOUSEEVENTF_MOVE`（dx/dy 像素偏移）
- 按钮：左键 / 右键 / 中键 / X1 / X2 的按下与释放
- 滚轮：`MOUSEEVENTF_WHEEL`（垂直滚动，120 = 一格）
- 水平滚轮：`MOUSEEVENTF_HWHEEL`

**键盘能力：**
- 虚拟键码按下/释放：`KEYBDINPUT.wVk` + `KEYEVENTF_KEYDOWN/KEYUP`
- 硬件扫描码：`KEYBDINPUT.wScan` + `KEYEVENTF_SCANCODE`（绕过某些键码映射问题）
- Unicode 文本：`KEYBDINPUT.wScan` + `KEYEVENTF_UNICODE`（直接发送 Unicode 字符）

**限制：**
- UIPI 阻止低权限进程向高权限窗口（如管理员运行的记事本）发送输入
- 部分游戏使用 Raw Input / DirectInput 绕过系统输入队列，SendInput 无效

#### 2. 窗口消息层 — `PostMessage`（已有基础，完善即可）

```
PostMessage(hWnd, WM_LBUTTONDOWN, ... ) → 目标窗口消息队列 → DispatchMessage → 窗口过程
                  ↑ 直接投递，不经过系统输入队列
```

**鼠标消息：**
- `WM_LBUTTONDOWN/UP`, `WM_RBUTTONDOWN/UP`, `WM_MBUTTONDOWN/UP`
- `WM_MOUSEMOVE`
- `WM_MOUSEWHEEL`, `WM_MOUSEHWHEEL`（滚轮）
- `WM_XBUTTONDOWN/UP`（侧键）

**键盘消息：**
- `WM_KEYDOWN/UP`, `WM_SYSKEYDOWN/UP`（Alt 组合键）
- `WM_CHAR`（字符输入）
- `WM_DEADCHAR`（死键，如 ^¨）

**限制：**
- 很多应用依赖 `GetKeyState()` / `GetAsyncKeyState()` / `GetCursorPos()` 判断真实输入状态
- Raw Input 应用完全忽略窗口消息
- 鼠标消息坐标是 client 坐标，与 SendInput 的 screen 坐标不同

#### 3. UI自动化层 — Windows Automation API

```
IUIAutomation → ElementFromPoint / FindFirst → InvokePattern.Invoke() / ValuePattern.SetValue()
```

**适用场景：**
- 标准 Win32 控件（Button, Edit, ComboBox, ListView 等）
- WPF / UWP 控件（自带 UIA Provider）
- 浏览器内容（通过浏览器 UIA 实现）

**限制：**
- 自定义渲染窗口（游戏、Electron、Qt 非原生控件）通常没有 UIA Provider
- 不适合通用输入转发，更适合特定控件自动化

**本次：不作为主力方案。** 当前需求是通用输入转发（包括游戏窗口），UIA 覆盖面不足。
后期如果有特定 UI 自动化需求（如网页表单填写），可单独实现为辅助方法。

#### 4. 驱动层 — 虚拟 HID 驱动

**两种方案：**

| 方案 | 原理 | 成熟度 |
|------|------|--------|
| **Interception** | `interception.dll` + 内核驱动，拦截/注入键鼠 | 开源，社区活跃，有签名的驱动 |
| **vmulti / 自研 HID** | 注册虚拟 HID 设备（鼠标/键盘），发送 HID Report | 需要 Windows 驱动签名，开发复杂 |

**Interception 特点：**
- `interception_send(context, device, &stroke, 1)` 发送输入
- 系统将其视为真实硬件设备输入
- 可指定目标设备（按 hardware ID 过滤）
- 驱动有 EV 签名，Win10/11 可加载

**限制：**
- 需要安装驱动（管理员权限一次性操作）
- 内核驱动兼容性（Windows 更新可能影响）
- 开发/调试难度高

**本次：保留 `driver` 选项，但本次不实现。** 等 SendInput 无法满足需求时再启用。

---

## 二、实现计划

### Phase 1 — C++ 层：扩展 `cmd_send_input` 支持完整输入类型

**文件：** `monitor_app/src/commands.cpp` — `cmd_send_input` 函数

#### 1.1 新增输入类型

```
type: "click"       → 现有，保留
type: "dblclick"    → 双击（两次 click，间隔 GetDoubleClickTime()）
type: "move"        → 现有，保留
type: "drag"        → 新：mousedown → 拖拽路径 → mouseup
type: "wheel"       → 新：MOUSEEVENTF_WHEEL / WM_MOUSEWHEEL
type: "keydown"     → 新：单个按键按下
type: "keyup"       → 新：单个按键释放
type: "keypress"    → 新：按键按下+释放（常用字符输入场景）
type: "text"        → 新：Unicode 文本字符串（逐字发送）
type: "combo"       → 新：组合键（如 Ctrl+C = Ctrl 按下 → C 按下 → C 释放 → Ctrl 释放）
```

#### 1.2 新增参数

```json
// mouse wheel
{ "type": "wheel", "delta": 120, "x_norm": 0.5, "y_norm": 0.5 }

// key down/up
{ "type": "keydown", "key": "A", "vk": 65, "scan": 30 }
{ "type": "keyup",   "key": "A", "vk": 65, "scan": 30 }

// text input (Unicode)
{ "type": "text", "text": "hello世界" }

// combo (modifiers + key)
{ "type": "combo", "keys": ["Ctrl", "C"] }

// drag (path of normalized coords)
{ "type": "drag", "button": "left", "path": [{x:0.5,y:0.5}, {x:0.6,y:0.5}, {x:0.7,y:0.6}] }

// dblclick
{ "type": "dblclick", "button": "left", "x_norm": 0.5, "y_norm": 0.5 }
```

#### 1.3 SendInput 键盘实现

```cpp
// 虚拟键码方式（最通用）
INPUT inputs[2] = {};
inputs[0].type = INPUT_KEYBOARD;
inputs[0].ki.wVk = vk;          // 如 'A' → 0x41
inputs[0].ki.wScan = scan;       // 硬件扫描码
inputs[0].ki.dwFlags = 0;       // KEYDOWN
inputs[1].type = INPUT_KEYBOARD;
inputs[1].ki.wVk = vk;
inputs[1].ki.wScan = scan;
inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
SendInput(2, inputs, sizeof(INPUT));

// Unicode 方式（不受键盘布局影响）
inputs[0].ki.wVk = 0;
inputs[0].ki.wScan = ch;         // UTF-16 码元
inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
SendInput(1, inputs, sizeof(INPUT));
```

#### 1.4 组合键实现

```cpp
// Ctrl+C:
// 1. Ctrl down  (VK_CONTROL)
// 2. C down     (0x43)
// 3. C up       (0x43 | KEYEVENTF_KEYUP)
// 4. Ctrl up    (VK_CONTROL | KEYEVENTF_KEYUP)
INPUT combo[4] = { ... };
SendInput(4, combo, sizeof(INPUT));
```

#### 1.5 拖拽实现

```cpp
// 1. 移动到起点 → MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE
// 2. 按下按钮 → MOUSEEVENTF_LEFTDOWN
// 3. 遍历路径点 → MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE
// 4. 释放按钮 → MOUSEEVENTF_LEFTUP
```

---

### Phase 2 — 前端层：MonitorView 完整交互捕获

**文件：** `monitor_web/src/components/MonitorView.tsx`

#### 2.1 状态机

```
IDLE ──mousedown──→ DRAGGING ──mousemove──→ DRAGGING（累积路径点）
  │                      │
  │                      └──mouseup──→ IDLE（发送 drag 指令）
  └──click────────→ IDLE（发送 click 指令）
  └──dblclick─────→ IDLE（发送 dblclick 指令）
  └──wheel────────→ 发送 wheel 指令，保持 IDLE
  └──keydown──────→ 发送 keydown 指令，保持 IDLE（预览画布有焦点时）
```

#### 2.2 新增交互

| 用户操作 | 前端捕获 | C++ 指令 |
|---------|---------|---------|
| 单击 | `onClick` | `click` |
| 双击 | `onDoubleClick` | `dblclick` |
| 拖拽 | `mousedown → mousemove* → mouseup` | `drag`（含路径点数组） |
| 滚轮 | `onWheel` | `wheel`（含 delta） |
| 按键 | `keydown/keyup`（画布聚焦时） | `keydown` / `keyup` / `keypress` |
| 组合键 | `keydown` 中检测 modifier | `combo`（如 `Ctrl+C`） |
| 文本输入 | `onPaste` / `input` 事件 | `text`（Unicode 文本） |

#### 2.3 键盘焦点

Monitor 预览画布添加 `tabIndex={0}`，用户点击画布后获得焦点，
`keydown`/`keyup`/`keypress` 事件直接转发。画布边框显示焦点状态（accent ring）。

按 `Escape` 或点击画布外部释放焦点。

#### 2.4 可视反馈

| 操作 | 视觉效果 |
|------|---------|
| 光标悬停 | 十字光标（已有） |
| 单击 | 涟漪圆圈（从点击位置扩散消失，300ms） |
| 双击 | 双层涟漪 |
| 拖拽中 | 半透明矩形选区（从起点到当前位置） |
| 拖拽完成 | 矩形闪烁后消失 |
| 滚轮 | 微小的向上/向下箭头飘过 |
| 按键 | 画布底部中央浮现按键名（如 `[Ctrl+C]`），1s 后消失 |
| 画布聚焦 | 边框 accent 色 + 底部提示 "已聚焦 — 键盘输入将转发到目标窗口" |

#### 2.5 组件结构

```tsx
// MonitorView 新增
const [focused, setFocused] = useState(false)
const [dragPath, setDragPath] = useState<{x:number,y:number}[] | null>(null)
const [clickRipples, setClickRipples] = useState<{x:number,y:number,id:number}[]>([])
const [keyToast, setKeyToast] = useState<string | null>(null)
const [wheelIndicator, setWheelIndicator] = useState<{delta:number,id:number} | null>(null)

// 事件处理
const handleMouseDown = useCallback(...)
const handleMouseMove = useCallback(...)  // only during drag
const handleMouseUp = useCallback(...)
const handleWheel = useCallback(...)
const handleKeyDown = useCallback(...)
const handleKeyUp = useCallback(...)
```

---

### Phase 3 — Settings + Keyboard Mapping

#### 3.1 键盘映射配置

新增 Settings → Capture → Keyboard 小节：

- **Key repeat（按键重复）：** ON/OFF，模拟按住键时的重复输入（跟随系统设置）
- **Repeat delay / rate：** 仅在 Key repeat ON 时显示（或跟随系统默认值）
- **Modifier passthrough：** 哪些修饰键转发（默认全部：Ctrl, Alt, Shift, Win）

#### 3.2 Input Method 说明更新

| 方法 | 鼠标 | 键盘 | 拖拽 | 滚轮 |
|------|------|------|------|------|
| SendInput | ✅ | ✅ | ✅ | ✅ |
| PostMessage | ✅ | ✅ | ✅ | ✅ |
| Driver | ❌ 未实现 | ❌ 未实现 | ❌ 未实现 | ❌ 未实现 |

---

### Phase 4 — 数据流总结

```
用户操作在 Monitor 预览画布上
    │
    ├─ 鼠标事件（click/drag/wheel）
    │   └─ getBoundingClientRect → 归一化坐标 (0-1)
    │       └─ hostCall('send_input', {hwnd, type, button, x_norm, y_norm, ...})
    │           └─ C++ cmd_send_input
    │               ├─ SendInput: 归一化坐标 → 虚拟桌面绝对坐标(0-65535)
    │               └─ PostMessage: 归一化坐标 → Client 坐标 → LPARAM
    │
    └─ 键盘事件
        └─ 画布聚焦时，keydown/keyup → e.key, e.code, e.keyCode
            └─ hostCall('send_input', {hwnd, type:'keydown', key, vk, scan, ...})
                └─ C++ cmd_send_input
                    ├─ SendInput: vk + scan → KEYBDINPUT
                    └─ PostMessage: vk → WM_KEYDOWN/UP + WM_CHAR
```

---

## 三、实现顺序

| 步骤 | 内容 | 文件 | 预计工时 |
|------|------|------|---------|
| 1 | C++ 扩展 `cmd_send_input`：keydown/keyup/keypress/wheel/drag/dblclick/combo/text | `commands.cpp` | 核心 |
| 2 | C++ `json_helper` 增加数组解析（drag path） | `json_helper.h` | 小 |
| 3 | 前端 MonitorView 交互状态机（drag/wheel/key 事件） | `MonitorView.tsx` | 核心 |
| 4 | 前端可视反馈（涟漪/选区/按键提示/焦点环） | `MonitorView.tsx` + 独立 effect | 视觉 |
| 5 | 前端 Settings 键盘选项 + 说明更新 | `SettingsView.tsx` + `constants.ts` | 小 |
| 6 | 前端 App.tsx 连接新状态 | `App.tsx` | 小 |

---

## 四、需要确认的问题

1. **驱动层：** 本次只保留占位，不实现 Interception。确认？
2. **键盘聚焦方式：** 点击画布自动聚焦 + 键盘转发 → 目标窗口。需要一种方式退出（Escape 或点击外部）。确认？
3. **拖拽路径采样：** 拖拽时每个 mousemove 都采样会产很多点。建议每 50ms 最多发一次（即 20 点/秒，足够平滑）。确认？
4. **文本输入：** Unicode 逐字发送 vs 整个字符串一次发送？SendInput 逐字更可靠（KEYEVENTF_UNICODE + 微延迟），但长文本慢。建议两种都支持：短文本走 `keypress`，长文本走 `text`（内部逐字 + Sleep(5)）。确认？
5. **修饰键状态同步：** 前端按 Ctrl 但没按 C 就松开了 → C++ 要不要发送 Ctrl up？建议前端跟踪 modifier 状态，mouseup 在画布外时自动释放所有按下键。
6. **预览窗口坐标精度：** 当前 click 用 `getBoundingClientRect` 归一化。拖拽和滚轮用同样方式。但如果 canvas 有 letterbox（aspect-ratio 黑边），归一化坐标需要映射到实际画面区域而非整个 div。是否需要处理？

---

请逐一确认，或直接说 "全部确认" 开始实现。
