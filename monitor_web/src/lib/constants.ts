// ═══ Shared constants ═══

// Collapsible card header (used in 6 places)
export const COLLAPSIBLE_HEADER =
  'w-full flex items-center justify-between px-3 py-2 hover:bg-bg-hover cursor-pointer transition-colors outline-none'

// Selectable option button (capture method + transport)
export const SELECTABLE_BTN =
  'flex items-center w-full px-3 py-2 rounded-lg border transition'

// Methods that cannot capture minimized windows
export const METHOD_SHORT: Record<string, string> = {
  wgc: 'WGC', gdi: 'GDI', dxgi: 'DXGI', printwindow: 'PW',
  screenbitblt: 'SBlt', GDI: 'GDI', 'GDI(GetWindowDC)': 'GDI',
  PrintWindow: 'PW', 'PrintWindow(minimized)': 'PW',
  ScreenBitBlt: 'SBlt', DesktopBlt: 'DXGI', WGC: 'WGC',
}

export const METHODS_NO_MINIMIZED = ['wgc', 'gdi', 'printwindow', 'screenbitblt']

export const cantCaptureMinimized = (method: string, ws: string) =>
  ws === 'minimized' && METHODS_NO_MINIMIZED.includes(method)

export const STATE_LABEL: Record<string, string> = {
  desktop: '桌面', foreground: '前台', background: '后台',
  minimized: '最小化', hidden: '隐藏', closed: '已关闭', unknown: '未知',
}

export const STATE_COLOR: Record<string, string> = {
  desktop: 'text-text-muted', foreground: 'text-success',
  background: 'text-accent', minimized: 'text-error',
  hidden: 'text-error', closed: 'text-error', unknown: 'text-text-muted',
}

export const CAPTURE_METHODS = [
  { v: 'wgc',  name: 'WGC', eng: 'GPU FramePool', rec: '前台/后台/桌面', desc: 'GPU 加速，支持后台/遮挡窗口，前台后台及桌面首选' },
  { v: 'dxgi', name: 'DXGI', eng: 'DesktopBlt',   rec: '桌面/最小化', desc: '全桌面 GDI 位图，最小化窗口时唯一可行方案' },
]

export const RENDER_METHODS = [
  { v: 'shared', name: 'SharedBuffer', eng: 'Zero-copy COM', rec: '当前', desc: 'C++ COM SharedBuffer → JS ArrayBuffer → Canvas putImageData，零拷贝无编解码' },
  { v: 'h264',   name: 'H.264',        eng: 'GPU MFT + MSE', rec: '未实现', desc: 'GPU MFT 硬件编码 → fMP4 分片 → MSE → <video> 标签，低延迟高压缩' },
  { v: 'h265',   name: 'H.265',        eng: 'GPU MFT + MSE', rec: '未实现', desc: 'HEVC 硬件编码，压缩率更高但兼容性有限，需 Windows 11 + HEVC 扩展' },
]

export const CAPTURE_MODES = [
  { v: 'foreground', label: '前台 (Foreground)', desc: '窗口可见且在最前 → 推荐 WGC GPU 加速', method: 'wgc' },
  { v: 'background', label: '后台 (Background)', desc: '窗口被遮挡但未最小化 → 推荐 WGC (唯一支持后台)', method: 'wgc' },
  { v: 'minimized',  label: '最小化 (Minimized)',  desc: '窗口已最小化 → 只能用 DesktopGDI 截桌面', method: 'dxgi' },
]

export const INPUT_METHODS = [
  { v: 'sendinput',  name: 'SendInput',  eng: '应用层', rec: '推荐', desc: 'SendInput API 合成系统输入，与真实硬件走相同路径。支持单击/双击/拖拽/滚轮/键盘/组合键/Unicode文本。兼容性最好，受UIPI限制。' },
  { v: 'winapi',     name: 'WinAPI',     eng: 'OS层',   rec: '进阶', desc: 'AttachThreadInput 挂接线程 + SetForegroundWindow 激活窗口 + SendMessage 同步投递。正确更新目标线程输入状态，绕过部分UIPI限制。' },
  { v: 'postmessage', name: 'PostMessage', eng: '窗口消息层', rec: '备选', desc: '直接向目标窗口队列投递 WM_* 消息，异步非阻塞。部分应用依赖实时输入状态可能无响应。' },
  { v: 'driver',      name: 'Driver',     eng: '驱动层', rec: '未实现', desc: 'Interception/虚拟HID内核级注入。系统视为真实硬件，完全绕过UIPI。需安装驱动，后期迭代。' },
]
