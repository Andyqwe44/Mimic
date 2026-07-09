import { useState, useEffect, useRef, useCallback } from 'react'
import { createPortal } from 'react-dom'
import { Play, Square, Camera, Monitor, Settings, Moon, Sun, ChevronDown, ChevronLeft, FileText, X, MonitorUp, Search, MonitorSmartphone, RefreshCw, FolderOpen, Cpu, Pencil, Copy, Check, ArrowDown, Unlink, Pin } from 'lucide-react'
// ── WebView2 WebMessage bridge (replaces Tauri invoke) ──
type PendingCall = {
  resolve: (value: any) => void;
  reject: (reason: any) => void;
  timer: ReturnType<typeof setTimeout>;
};
let _callId = 0;
const _pending = new Map<number, PendingCall>();

// Listen for responses from C++ host
if (typeof (window as any).chrome?.webview !== 'undefined') {
  (window as any).chrome.webview.addEventListener('message', (e: any) => {
    try {
      // PostWebMessageAsJson sends a pre-parsed object, not a JSON string
      const msg = typeof e.data === 'string' ? JSON.parse(e.data) : e.data;
      // Real-time log push from C++ (capture_log_set_notify callback)
      if (msg.type === 'log') {
        logMgr.addRemote(msg.ts, msg.tag, msg.msg);
        return;
      }
      const pending = _pending.get(msg.id);
      if (pending) {
        clearTimeout(pending.timer);
        _pending.delete(msg.id);
        pending.resolve(msg.result);
      }
    } catch {}
  });
}

// Replacement for invoke()
function hostCall(cmd: string, args?: Record<string, any>): Promise<any> {
  return new Promise((resolve, reject) => {
    const id = ++_callId;
    const timer = setTimeout(() => {
      _pending.delete(id);
      reject(new Error(`hostCall timeout: ${cmd}`));
    }, 30000);
    _pending.set(id, {
      resolve: (raw: any) => {
        // Unwrap {id, result} envelope — handle both wrapped and raw formats
        resolve(raw && typeof raw === 'object' && 'result' in raw ? raw.result : raw);
      },
      reject,
      timer
    });
    try {
      (window as any).chrome.webview.postMessage(JSON.stringify({ cmd, id, args: args || {} }));
    } catch (e) {
      clearTimeout(timer);
      _pending.delete(id);
      reject(e);
    }
  });
}

// ═══ Tooltip ── 300ms delay, portal to body, smart positioning ═══
function Tooltip({ text, children, className }: { text: string; children: React.ReactElement; className?: string }) {
  const [show, setShow] = useState(false)
  const [pos, setPos] = useState({ x: 0, y: 0, placement: 'top' as 'top'|'bottom' })
  const timer = useRef<number>(0)
  const ref = useRef<HTMLDivElement>(null)

  const updatePos = () => {
    if (!ref.current) return
    const r = ref.current.getBoundingClientRect()
    // Estimate tooltip size: ~200px wide, ~28px tall
    const tipW = Math.min(text.length * 8 + 20, 300)
    const tipH = 28
    // Place above unless too close to top edge
    const above = r.top > tipH + 8
    let x = r.left + r.width / 2
    // Clamp horizontal so tip doesn't overflow viewport
    const vw = window.innerWidth
    x = Math.max(tipW / 2 + 4, Math.min(vw - tipW / 2 - 4, x))
    setPos({ x, y: above ? r.top : r.bottom, placement: above ? 'top' : 'bottom' })
  }

  return (
    <div ref={ref} className={`relative inline-flex ${className || ''}`}
      onMouseEnter={() => { updatePos(); timer.current = window.setTimeout(() => { updatePos(); setShow(true) }, 300) }}
      onMouseLeave={() => { clearTimeout(timer.current); setShow(false) }}
      onMouseMove={() => { if (!show) { clearTimeout(timer.current); timer.current = window.setTimeout(() => { updatePos(); setShow(true) }, 300) } }}>
      {children}
      {show && createPortal(
        <div className="fixed px-2 py-1 bg-bg-tertiary text-text-primary text-xs rounded shadow-lg whitespace-nowrap pointer-events-none z-[9999]"
          style={{
            left: pos.x,
            top: pos.placement === 'top' ? pos.y - 6 : pos.y + 6,
            transform: pos.placement === 'top' ? 'translate(-50%, -100%)' : 'translate(-50%, 0)'
          }}>
          {text}
        </div>,
        document.body
      )}
    </div>
  )
}

// ═══ Layout ───
const MIN_LEFT_WIDTH = 360
const DEFAULT_RIGHT_WIDTH = 324

// Shared CSS: collapsible card header (used in 6 places)
const COLLAPSIBLE_HEADER = 'w-full flex items-center justify-between px-3 py-2 hover:bg-bg-hover cursor-pointer transition-colors outline-none'
// Shared CSS: selectable option button (capture method + transport)
const SELECTABLE_BTN = 'flex items-center w-full px-3 py-2 rounded-lg border transition'

// ── Smooth theme switch: add class for 200ms, all elements animate in sync ──
function applyTheme(isDark: boolean) {
  document.documentElement.classList.add('theme-switching')
  document.documentElement.classList.toggle('dark', isDark)
  setTimeout(() => document.documentElement.classList.remove('theme-switching'), 220)
}

// ═══ Theme btn ───
function ThemeBtn({ dark, onToggle }: { dark: boolean; onToggle: () => void }) {
  return (
    <Tooltip text={dark ? "切换亮色主题" : "切换暗色主题"}>
      <button onClick={onToggle}
        className="p-2 rounded-md hover:bg-bg-hover transition-colors">
        {dark ? <Sun className="w-4 h-4 text-text-secondary" /> : <Moon className="w-4 h-4 text-text-secondary" />}
      </button>
    </Tooltip>
  )
}

// ═══ Reusable components with REQUIRED title (compile-time enforced) ═══
function ActionBtn({ icon, label, title, variant, onClick, className }: {
  icon: React.ReactNode; label: string; title: string;
  variant: 'primary' | 'danger' | 'outline';
  onClick?: () => void; className?: string;
}) {
  const wide = label.length > 10
  return (
    <Tooltip text={title}>
      <button onClick={onClick}
        className={`inline-flex items-center justify-center gap-1.5 rounded-md px-2.5 h-7 text-xs font-medium transition-all duration-150 ${
          wide ? 'min-w-[120px]' : 'w-20'
        } ${className ?? ''} ${
          variant === 'primary' ? 'bg-accent text-white hover:bg-accent-hover'
          : variant === 'danger' ? 'bg-error/20 text-error hover:bg-error/30'
          : 'border border-border text-text-secondary hover:bg-bg-hover'
        }`}>
        {icon}<span>{label}</span>
      </button>
    </Tooltip>
  )
}

// ═══ TopBar (MXU-style tab bar) ───
function TopBar({ tab, setTab, running, onStart, onStop, dark, onToggleTheme }: {
  tab: string; setTab: (t: 'Monitor'|'Log'|'Settings') => void; running: boolean; onStart: () => void; onStop: () => void;
  dark: boolean; onToggleTheme: () => void;
}) {
  const tabs = [
    { id: 'Monitor' as const, icon: <Monitor className="w-3.5 h-3.5" />, label: 'Monitor' },
    { id: 'Log' as const, icon: <FileText className="w-3.5 h-3.5" />, label: 'Log' },
    { id: 'Settings' as const, icon: <Settings className="w-3.5 h-3.5" />, label: 'Settings' },
  ]
  return (
    <div className="flex items-center h-10 bg-bg-secondary border-b border-border select-none shrink-0">
      <div className="flex-1 flex items-center h-full overflow-x-auto">
        {tabs.map(t => (
          <button key={t.id} onClick={() => { setTab(t.id); addLog(`[Tab] ${t.label}`) }}
            className={`group flex items-center gap-1.5 h-full px-3 cursor-pointer border-r border-border border-b-[3px] min-w-[100px] transition-colors
              ${t.id === tab ? 'bg-bg-primary text-accent border-b-accent' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover border-b-transparent'}`}>
            {t.icon}
            <span className="text-sm font-medium">{t.label}</span>
          </button>
        ))}
      </div>
      <div className="flex items-center gap-1 px-2">
        {running
          ? <ActionBtn icon={<Square className="w-3.5 h-3.5" />} label="Stop" title="停止所有运行中的任务" variant="danger" onClick={() => { onStop(); addLog('[Action] Stop') }} />
          : <ActionBtn icon={<Play className="w-3.5 h-3.5" />} label="Start" title="启动agent任务" variant="primary" onClick={() => { onStart(); addLog('[Action] Start') }} />
        }
        <div className="mx-1 h-4 w-px bg-border" />
        <ThemeBtn dark={dark} onToggle={onToggleTheme} />
      </div>
    </div>
  )
}

// ═══ BottomBar ───
function BottomBar({ running, fps, lat }: { running: boolean; fps: number; lat: number }) {
  return (
    <div className="flex items-center h-10 bg-bg-secondary border-t border-border px-4 shrink-0">
      <div className="flex items-center gap-3 text-xs text-text-secondary">
        <span className={`inline-block w-2 h-2 rounded-full ${running ? 'bg-success' : 'bg-text-muted'}`} />
        <span>{running ? 'Running' : 'Idle'}</span>
        <span className="text-border">|</span>
        <span>FPS: {fps}</span>
        <span className="text-border">|</span>
        <span>Lat: {lat}ms</span>
      </div>
      <div className="flex-1" />
      <span className="text-xs text-text-muted">github.com/Andyqwe44/tictactoe</span>
    </div>
  )
}

// ═══ WindowInfo type ───
interface WindowInfo { title: string; category: string; hwnd: number; desktop?: number }
// ═══ Window Picker ───
// ═══ Capture Mode Picker ───
const CAPTURE_MODES = [
  { v: 'foreground', label: '前台 (Foreground)', desc: '窗口可见且在最前 → 推荐 WGC GPU 加速', method: 'wgc' },
  { v: 'background', label: '后台 (Background)', desc: '窗口被遮挡但未最小化 → 推荐 WGC (唯一支持后台)', method: 'wgc' },
  { v: 'minimized',  label: '最小化 (Minimized)',  desc: '窗口已最小化 → 只能用 DesktopGDI 截桌面', method: 'dxgi' },
]

const PICKER_W = 'w-[520px]'
const PICKER_MAXH = 'max-h-[min(560px,85vh)] min-h-[min(560px,85vh)]'

function TargetPickerModal({ open, onClose, onSelectWindow, onSelectMode }: {
  open: boolean
  onClose: () => void
  onSelectWindow: (w: WindowInfo) => void
  onSelectMode: (method: string, expectedState: string) => void
}) {
  const [page, setPage] = useState<'window' | 'mode'>('window')
  const [animReady, setAnimReady] = useState(false)
  // ── page 1: window picker state ──
  const [search, setSearch] = useState('')
  const [windows, setWindows] = useState<WindowInfo[]>([])
  const [filter, setFilter] = useState('all')
  const [loading, setLoading] = useState(false)
  const [currentDesktop, setCurrentDesktop] = useState(1)
  // ── page 2: pending window (awaiting mode) ──
  const [pendingWin, _setPendingWin] = useState<WindowInfo | null>(null)

  // Reset on open — delay transition enable so stale page state doesn't animate
  useEffect(() => {
    if (open) {
      setPage('window'); setFilter('all'); setSearch('')
      loadWindows(); loadCurrentDesktop()
      const id = requestAnimationFrame(() => setAnimReady(true))
      return () => cancelAnimationFrame(id)
    } else {
      setAnimReady(false)
    }
  }, [open])

  const loadWindows = async () => {
    setLoading(true)
    try {
      const list = await hostCall('list_windows')
      setWindows(list)
      addLog(`[Window] loaded ${list.length} entries`)
    } catch {
      setWindows([{ title: ' Entire Desktop', category: 'desktop', hwnd: 0 }, { title: 'Tic Tac Toe — main.exe', category: 'window', hwnd: 0 }, { title: 'Notepad', category: 'window', hwnd: 0 }, { title: 'Chrome', category: 'window', hwnd: 0 }])
    }
    setLoading(false)
  }

  const loadCurrentDesktop = async () => {
    try {
      const list = await hostCall('list_desktops')
      const cur = list.find((d: any) => d.current)
      if (cur) setCurrentDesktop(cur.index)
    } catch { /* use default 1 */ }
  }

  const categories = ['all', 'desktop', 'window'] as const
  const filtered = windows.filter(w => {
    if (filter === 'desktop') return w.category === 'desktop'
    if (filter === 'window') return w.category === 'window'
    return true
  }).filter(w => w.title.toLowerCase().includes(search.toLowerCase()))

  const handlePickWindow = async (w: WindowInfo) => {
    onSelectWindow(w)
    addLog(`[Window] ${w.title}`)
    // If target is on a different desktop, switch first
    const winDesktop = w.desktop || 1
    if (w.desktop != null && winDesktop !== currentDesktop && w.category !== 'desktop') {
      try {
        await hostCall('switch_desktop', { index: winDesktop - 1 })
        addLog(`[Desktop] switched to D${winDesktop}`)
      } catch { /* continue anyway */ }
    }
    if (w.hwnd === 0 || w.category === 'desktop') {
      onSelectMode('dxgi', 'desktop')
      // addLog('[Capture] desktop → dxgi')
      onClose()
    } else {
      // DEV: skip mode picker page 2 — directly use wgc/foreground
      onSelectMode('wgc', 'foreground')
      // addLog('[Capture] auto → wgc (mode picker skipped)')
      onClose()
      // setPendingWin(w)   // ← page 2 code preserved, not deleted
      // setPage('mode')
    }
  }

  const handlePickMode = (method: string, expectedState: string) => {
    onSelectMode(method, expectedState)
    // addLog(`[Capture] mode=${expectedState} → ${method}`)
    onClose()
  }

  const handleBack = () => { setPage('window') }

  if (!open) return null

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center">
      <div className="absolute inset-0 bg-black/30" onClick={() => { onClose(); addLog('[Window] picker cancelled') }} />
      <div className={`relative ${PICKER_W} ${PICKER_MAXH} bg-bg-secondary border border-border rounded-xl shadow-lg overflow-hidden`}>
        <div className={`flex ${animReady ? 'transition-transform duration-300 ease-out' : ''}`}
          style={{ transform: page === 'window' ? 'translateX(0)' : 'translateX(-100%)' }}>
          {/* ═══ Page 1: Window Picker ═══ */}
          <div className={`${PICKER_W} flex-shrink-0 flex flex-col ${PICKER_MAXH}`}>
            <div className="flex items-center justify-between px-4 py-3 border-b border-border">
              <div className="flex items-center gap-2">
                <MonitorUp className="w-4 h-4 text-accent" />
                <span className="text-sm font-semibold text-text-primary">Select Target</span>
              </div>
              <Tooltip text="关闭"><button onClick={() => { onClose(); addLog('[Window] picker cancelled') }} className="p-1 rounded-md hover:bg-bg-hover transition-colors"><X className="w-4 h-4 text-text-secondary" /></button></Tooltip>
            </div>
            <div className="flex items-center gap-1 px-4 pt-3 pb-1">
              {categories.map(c => (
                <Tooltip key={c} text={`筛选: ${c === 'all' ? '全部' : c === 'desktop' ? '桌面' : '窗口'}`}>
                  <button onClick={() => { setFilter(c); addLog(`[Filter] ${c}`) }}
                    className={`px-3 py-1 rounded-full text-xs font-medium transition-colors capitalize
                      ${filter === c ? 'bg-accent text-white' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'}`}>
                    {c === 'all' ? 'All' : c === 'desktop' ? ' Desktop' : ' Windows'}
                  </button>
                </Tooltip>
              ))}
              <div className="flex-1" />
              <Tooltip text="刷新窗口列表">
                <button onClick={() => { loadWindows(); addLog('[Window] refreshing list') }}
                  className={`p-1.5 rounded-md hover:bg-bg-hover transition-colors text-text-secondary ${loading ? 'animate-spin' : ''}`}>
                  <RefreshCw className="w-3.5 h-3.5" />
                </button>
              </Tooltip>
            </div>
            <div className="px-4 py-2">
              <div className="flex items-center gap-2 h-8 rounded-lg border border-border bg-bg-primary px-3">
                <Search className="w-3.5 h-3.5 text-text-muted shrink-0" />
                <input value={search} onChange={e => setSearch(e.target.value)} placeholder="Search..."
                  className="flex-1 bg-transparent text-sm text-text-primary outline-none placeholder:text-text-muted" autoFocus />
              </div>
            </div>
            <div className="flex-1 overflow-y-auto px-2 pb-2">
              {loading && filtered.length === 0 ? (
                <div className="flex items-center justify-center py-8 text-sm text-text-muted">Loading...</div>
              ) : (
                <div className="grid grid-cols-2 gap-1">
                {filtered.map((w) => {
                  const self = w.title.includes('Game Agent Monitor')
                  const winDesktop = w.desktop || 1
                  const isRemote = w.desktop != null && winDesktop !== currentDesktop && w.category !== 'desktop'
                  return (
                  <Tooltip key={`${w.hwnd}-${w.category}`} text={self ? '自身窗口，禁止捕获' : isRemote ? `选择: ${w.title}（将切换到 D${winDesktop}）` : `选择: ${w.title}`}>
                    <button
                      disabled={self}
                      onClick={() => handlePickWindow(w)}
                    className={`w-full flex items-center gap-2 px-2 py-2 rounded-lg text-left transition-colors group min-w-0 relative
                      ${self ? 'opacity-40 cursor-not-allowed' : 'hover:bg-bg-hover cursor-pointer'}`}>
                    {w.category === 'desktop' ? <MonitorSmartphone className="w-3.5 h-3.5 text-text-muted group-hover:text-accent shrink-0" />
                      : <Monitor className="w-3.5 h-3.5 text-text-muted group-hover:text-accent shrink-0" />}
                    <div className="flex-1 min-w-0">
                      <span className="text-xs text-text-primary truncate block">{w.title}</span>
                      <span className="text-xs text-text-muted capitalize">{w.category}</span>
                    </div>
                    {(w.desktop != null || w.category !== 'desktop') && (
                      <span className="absolute bottom-1 right-1 flex items-center gap-0.5 text-[10px] font-medium px-1.5 py-px rounded-full bg-accent-light text-accent whitespace-nowrap">
                        {isRemote && <Tooltip text="需切换桌面"><span>⚡</span></Tooltip>}
                        D{winDesktop || '?'}
                      </span>
                    )}
                  </button>
                  </Tooltip>
                )})}
                </div>
              )}
            </div>
            <div className="px-4 py-2 border-t border-border text-xs text-text-muted text-center">
              {(() => {
                const deskNums = new Set(windows.filter(w => (w.desktop || 0) > 0).map(w => w.desktop || 1))
                const deskCount = deskNums.size || 1
                return `${filtered.length} items — ${deskCount} desktop${deskCount !== 1 ? 's' : ''}, ${windows.filter(w => w.category === 'window').length} windows`
              })()}
            </div>
          </div>

          {/* ═══ Page 2: Capture Mode ═══ */}
          <div className={`${PICKER_W} flex-shrink-0 flex flex-col`}>
            <div className="flex items-center justify-between px-4 py-3 border-b border-border">
              <div className="flex items-center gap-2">
                <Tooltip text="返回窗口选择">
                  <button onClick={handleBack} className="p-1 rounded-md hover:bg-bg-hover transition-colors">
                    <ChevronLeft className="w-4 h-4 text-text-secondary" />
                  </button>
                </Tooltip>
                <MonitorUp className="w-4 h-4 text-accent" />
                <span className="text-sm font-semibold text-text-primary">Capture Mode</span>
              </div>
              <Tooltip text="关闭"><button onClick={() => { onClose(); addLog('[Capture] mode picker cancelled') }} className="p-1 rounded-md hover:bg-bg-hover transition-colors"><X className="w-4 h-4 text-text-secondary" /></button></Tooltip>
            </div>
            <div className="p-4 space-y-2">
              <div className="text-xs text-text-muted mb-3">
                目标: <span className="text-text-primary font-medium">{pendingWin?.title || ''}</span>
              </div>
              <div className="text-xs text-text-muted mb-2">请选择目标窗口的当前状态，系统将自动推荐最优捕获方案：</div>
              {CAPTURE_MODES.map(m => (
                <button key={m.v} onClick={() => handlePickMode(m.method, m.v)}
                  className="w-full flex flex-col items-start gap-0.5 px-3 py-2.5 rounded-lg text-left transition-colors hover:bg-bg-hover border border-border">
                  <span className="text-sm font-medium text-text-primary">{m.label}</span>
                  <span className="text-xs text-text-muted">{m.desc}</span>
                </button>
              ))}
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}

// ═══ Connection Panel ───

// Methods that cannot capture minimized windows
const METHOD_SHORT: Record<string,string> = { wgc:'WGC', gdi:'GDI', dxgi:'DXGI', printwindow:'PW', screenbitblt:'SBlt', GDI:'GDI', 'GDI(GetWindowDC)':'GDI', PrintWindow:'PW', 'PrintWindow(minimized)':'PW', ScreenBitBlt:'SBlt', DesktopBlt:'DXGI', WGC:'WGC' }
const METHODS_NO_MINIMIZED = ['wgc','gdi','printwindow','screenbitblt']
const cantCaptureMinimized = (method: string, ws: string) => ws === 'minimized' && METHODS_NO_MINIMIZED.includes(method)
const STATE_LABEL: Record<string,string> = { desktop:'桌面', foreground:'前台', background:'后台', minimized:'最小化', hidden:'隐藏', closed:'已关闭', unknown:'未知' }
const STATE_COLOR: Record<string,string> = { desktop:'text-text-muted', foreground:'text-success', background:'text-accent', minimized:'text-error', hidden:'text-error', closed:'text-error', unknown:'text-text-muted' }

const CAPTURE_METHODS = [
  { v: 'wgc',  name: 'WGC', eng: 'GPU FramePool', rec: '前台/后台/桌面', desc: 'GPU 加速，支持后台/遮挡窗口，前台后台及桌面首选' },
  { v: 'dxgi', name: 'DXGI', eng: 'DesktopBlt',   rec: '桌面/最小化', desc: '全桌面 GDI 位图，最小化窗口时唯一可行方案' },
]

const RENDER_METHODS = [
  { v: 'shared', name: 'SharedBuffer', eng: 'Zero-copy COM', rec: '当前', desc: 'C++ COM SharedBuffer → JS ArrayBuffer → Canvas putImageData，零拷贝无编解码' },
  { v: 'h264',   name: 'H.264',        eng: 'GPU MFT + MSE', rec: '未实现', desc: 'GPU MFT 硬件编码 → fMP4 分片 → MSE → <video> 标签，低延迟高压缩' },
  { v: 'h265',   name: 'H.265',        eng: 'GPU MFT + MSE', rec: '未实现', desc: 'HEVC 硬件编码，压缩率更高但兼容性有限，需 Windows 11 + HEVC 扩展' },
]

// ═══ Connection Panel (MXU-style collapsible card) ═══
function ConnectionPanel({ onSelect, onDisconnect, setSnapMethod, streamMethod, setStreamMethod, selWin, winState, expectedCaptureState, setExpectedCaptureState, expanded, onToggle, pinned, onTogglePin }: { onSelect: (w: WindowInfo) => void; onDisconnect?: () => void; snapMethod: string; setSnapMethod?: (m: string) => void; streamMethod: string; setStreamMethod?: (m: string) => void; selWin?: WindowInfo; winState: string; expectedCaptureState?: string; setExpectedCaptureState?: (s: string) => void; expanded: boolean; onToggle: () => void; pinned?: boolean; onTogglePin?: () => void }) {
  const [pickerOpen, setPickerOpen] = useState(false)
  const [selTitle, setSelTitle] = useState(' Entire Desktop')
  const [ip, setIp] = useState('127.0.0.1')
  const [port, setPort] = useState('9999')
  const isDesktop = selTitle === ' Entire Desktop'

  const handleSelectWindow = (w: WindowInfo) => {
    setSelTitle(w.title); onSelect(w)
  }

  const handleSelectMode = (method: string, expectedState: string) => {
    // Mode picker sets both methods (user's explicit choice)
    if (setSnapMethod) { setSnapMethod(method) }
    if (setStreamMethod) { setStreamMethod(method) }
    if (setExpectedCaptureState) { setExpectedCaptureState(expectedState) }
  }

  // Sync selTitle when parent changes selWin externally
  useEffect(() => {
    if (selWin && selWin.title !== selTitle) {
      setSelTitle(selWin.title)
    }
  }, [selWin?.title])

  const cantCapture = !isDesktop && cantCaptureMinimized(streamMethod, winState)
  const recommendedMethod = winState === 'minimized' ? 'dxgi' : 'wgc'


  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
      <div role="button" tabIndex={0} onClick={() => { onToggle(); addLog(`[Connection] ${!expanded ? 'expanded' : 'collapsed'}`) }} onKeyDown={e=>{if(e.key==='Enter'||e.key===' '){(e.currentTarget as HTMLElement).click()}}}
        className={COLLAPSIBLE_HEADER}>
        <div className="flex items-center gap-2 min-w-0">
          <MonitorUp className="w-4 h-4 text-text-secondary shrink-0" />
          <span className="text-sm font-medium text-text-primary shrink-0">Connection</span>
        </div>
        <div className="flex items-center gap-2 ml-2">
          <div className="flex items-baseline gap-2">
            <span className="text-[10px] text-text-muted shrink-0">状态</span>
            <span className="text-[11px] font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded shrink-0">{STATE_LABEL[winState] || winState}</span>
            <span className="text-[10px] text-text-muted shrink-0">推荐</span>
            <span className="text-[11px] font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded shrink-0">{METHOD_SHORT[recommendedMethod] || recommendedMethod}</span>
            {cantCapture && <span className="text-xs text-error shrink-0">⚠</span>}
          </div>
          {onTogglePin && (
            <Tooltip text={pinned ? "取消固定" : "固定面板"}>
              <button onClick={e => { e.stopPropagation(); onTogglePin() }}
                className={`p-1 rounded-md transition-colors ${pinned ? 'text-accent hover:bg-bg-tertiary' : 'text-text-secondary hover:text-text-primary hover:bg-bg-tertiary'}`}>
                <Pin className={`w-3.5 h-3.5 ${pinned ? 'fill-current' : ''}`} />
              </button>
            </Tooltip>
          )}
          <ChevronDown className={`w-4 h-4 text-text-muted transition-transform duration-150 ${expanded?'rotate-180':''}`} />
        </div>
      </div>
      <div className="grid transition-[grid-template-rows] duration-150 ease-out"
        style={{ gridTemplateRows: expanded ? '1fr' : '0fr' }}>
        <div className="overflow-hidden min-h-0" data-layout-measure="">
          <div className="border-t border-border" />
          <div className="max-h-[360px] overflow-y-auto p-3 space-y-2">
            {cantCapture && (
              <div className="text-xs text-error bg-red-500/10 rounded-lg px-2 py-1.5">
                窗口已最小化，{streamMethod.toUpperCase()} 无法截取。请切换为 WGC 或将窗口恢复前台。
              </div>
            )}
            {/* DEV: expected state mismatch warning — disabled during development */}
            {false && expectedCaptureState && winState !== 'desktop' && expectedCaptureState !== winState && (
              <div className="text-xs text-amber-400 bg-amber-500/10 rounded-lg px-2 py-1.5 flex items-center gap-1.5">
                <span className="shrink-0">⚠</span>
                <span>预期状态: <b>{STATE_LABEL[expectedCaptureState || 'unknown'] || expectedCaptureState}</b>，实际状态: <b>{STATE_LABEL[winState] || winState}</b>。截图可能失败或画面异常。</span>
              </div>
            )}
            <div className="flex justify-between">
              <div className="flex items-center gap-1.5">
                <Tooltip text="已选择的目标窗口（只读，请用Select选择）">
                  <input value={selTitle} readOnly placeholder="Window Title"
                    className="w-36 h-8 rounded-lg border border-border bg-bg-primary px-2 text-xs outline-none cursor-default text-text-muted truncate" />
                </Tooltip>
                {onDisconnect && (
                  <Tooltip text="断开当前窗口连接，回到桌面">
                    <button onClick={() => { onDisconnect(); setSelTitle(' Entire Desktop') }}
                      className="h-8 w-8 flex items-center justify-center rounded-md bg-amber-500/10 hover:bg-amber-500/20 text-amber-400 border border-amber-500/20 transition-colors shrink-0">
                      <Unlink className="w-3.5 h-3.5" />
                    </button>
                  </Tooltip>
                )}
              </div>
              <ActionBtn icon={<MonitorUp className="w-3.5 h-3.5" />} label="Select" title="选择要捕获的窗口或桌面" variant="primary" onClick={() => { setPickerOpen(true); addLog('[Window] opening picker') }} className="h-8" />
            </div>
            <div className="flex justify-between">
              <Tooltip text="AI模型服务器IP地址">
                <input value={ip} onChange={e => { const v=e.target.value; if(v.includes('::')){const[a,b]=v.split('::',2);setIp(a.trim());if(b?.trim())setPort(b.trim())}else setIp(v) }} placeholder="IP Address"
                  className="w-[184px] h-8 rounded-lg border border-border bg-bg-primary px-2 text-xs text-text-primary outline-none focus:border-accent transition-colors placeholder:text-text-muted" />
              </Tooltip>
              <Tooltip text="Port端口号">
                <input value={port} onChange={e => { const v=e.target.value; if(v.includes('::')){const[a,b]=v.split('::',2);setPort(a.trim());if(b?.trim())setIp(b.trim())}else setPort(v) }} placeholder="Port"
                  className="w-20 h-8 rounded-lg border border-border bg-bg-primary px-2 text-xs text-text-primary outline-none focus:border-accent transition-colors placeholder:text-text-muted" />
              </Tooltip>
            </div>
          </div>
        </div>
      </div>
      <TargetPickerModal open={pickerOpen} onClose={() => setPickerOpen(false)} onSelectWindow={handleSelectWindow} onSelectMode={handleSelectMode} />
    </div>
  )
}

// ═══ Screenshot Panel ───
// Single Canvas for both snapshot and real-time preview.
// Frames arrive via SharedBuffer (zero-copy) → sharedbufferreceived event → putImageData.
function ScreenshotPanel({ screenRatio, expanded, onToggle, previewing, previewingRef, snapshotRef, snapshotStartRef, capMethod, onTakeSnapshot, onTogglePreview, pinned, onTogglePin, hasContentRef }: {
  selWin?: WindowInfo; screenRatio: number; snapMethod: string; streamMethod: string; renderMethod: string; winState: string; expanded: boolean; onToggle: () => void;
  previewing: boolean; previewingRef: React.MutableRefObject<boolean>; snapshotRef: React.MutableRefObject<boolean>; snapshotStartRef: React.MutableRefObject<number>;
  capMethod: string;
  onTakeSnapshot: () => void; onTogglePreview: () => void;
  pinned: boolean; onTogglePin: () => void;
  hasContentRef: React.MutableRefObject<boolean>;
}) {
  const [hasContent, setHasContent] = useState(false)
  // Sync hasContent to parent ref for auto-layout decisions
  useEffect(() => { hasContentRef.current = hasContent }, [hasContent])
  const [snapshotLatency, setSnapshotLatency] = useState<number | null>(null)
  const [fps, setFps] = useState(0)
  const framesRef = useRef(0)
  const lastFpsRef = useRef(Date.now())
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const [canvasDims, setCanvasDims] = useState({ w: 0, h: 0 })
  const handlerRef = useRef<((e: any) => void) | null>(null)

  // Reset fps and latency when previewing state changes
  useEffect(() => {
    setFps(0); framesRef.current = 0; lastFpsRef.current = Date.now()
    if (previewing) setSnapshotLatency(null)
  }, [previewing])

  // ── SharedBuffer → Canvas (shared by snapshot + stream) ──
  useEffect(() => {
    const wv = (window as any).chrome?.webview
    if (!wv) { addLog('[Screenshot] SharedBuffer not available'); return }
    let firstFrame = true
    const handler = (e: any) => {
      const active = previewingRef.current || snapshotRef.current
      if (!active) {
        // addLog(`[SB] event ignored — previewing=${previewingRef.current} snapshot=${snapshotRef.current}`)
        return
      }
      if (firstFrame) { firstFrame = false /* addLog('[Screenshot] first SharedBuffer frame received') */ }
      try {
        const buf: ArrayBuffer = e.getBuffer()
        const metaRaw = e.additionalData
        const meta: { w: number; h: number } = typeof metaRaw === 'string' ? JSON.parse(metaRaw) : metaRaw
        if (!meta.w || !meta.h || meta.w <= 0 || meta.h <= 0) { return }
        const pixelCount = meta.w * meta.h * 4
        if (buf.byteLength < pixelCount) { return }
        const imgData = new ImageData(
          new Uint8ClampedArray(buf, 0, pixelCount),
          meta.w, meta.h
        )
        const c = canvasRef.current
        if (c) {
          c.width = meta.w; c.height = meta.h
          setCanvasDims({ w: meta.w, h: meta.h })
          const ctx = c.getContext('2d')
          if (ctx) { ctx.putImageData(imgData, 0, 0) }
        }
        setHasContent(true)
        framesRef.current++
        const now = Date.now(); const elapsed = now - lastFpsRef.current
        if (elapsed >= 1000) { setFps(Math.round(framesRef.current * 1000 / elapsed)); framesRef.current = 0; lastFpsRef.current = now }
        // Snapshot: one shot, then stop consuming
        if (snapshotRef.current) {
          snapshotRef.current = false
          const latency = Date.now() - snapshotStartRef.current
          setSnapshotLatency(latency)
          // addLog(`[SB] snapshot consumed, latency=${latency}ms`)
        }
      } catch (ex: any) {
        addLog(`[SB] EXCEPTION: ${ex?.message || ex}`)
      }
    }
    handlerRef.current = handler
    wv.addEventListener('sharedbufferreceived', handler)
    // addLog('[Screenshot] SharedBuffer listener registered')
    return () => { wv.removeEventListener('sharedbufferreceived', handler) }
  }, [])

  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
      <div role="button" tabIndex={0} onClick={() => { onToggle(); addLog(`[Screenshot] ${!expanded ? 'expanded' : 'collapsed'}`) }} onKeyDown={e=>{if(e.key==='Enter'||e.key===' '){(e.currentTarget as HTMLElement).click()}}}
        className={COLLAPSIBLE_HEADER}>
        <div className="flex items-center gap-2 min-w-0">
          <Camera className="w-4 h-4 text-text-secondary shrink-0" />
          <span className="text-sm font-medium text-text-primary shrink-0">Screenshot</span>
          {capMethod && <span className="text-[11px] font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded shrink-0">{METHOD_SHORT[capMethod] || capMethod.toUpperCase()}</span>}
          {previewing && <span className="text-xs text-text-muted shrink-0">{fps} FPS</span>}
          {snapshotLatency !== null && !previewing && (
            <span className="text-xs text-text-muted shrink-0">{snapshotLatency}ms</span>
          )}
        </div>
        <div className="flex items-center gap-0.5 shrink-0">
          <Tooltip text="单帧截图">
            <button onClick={e => { e.stopPropagation(); onTakeSnapshot() }}
              className="p-1 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors">
              <Camera className="w-3.5 h-3.5" />
            </button>
          </Tooltip>
          {previewing
            ? <Tooltip text="停止实时预览"><button onClick={e => { e.stopPropagation(); onTogglePreview() }}
                className="p-1 rounded-md text-success hover:bg-bg-tertiary transition-colors"><Square className="w-3.5 h-3.5" /></button></Tooltip>
            : <Tooltip text="开始实时预览"><button onClick={e => { e.stopPropagation(); onTogglePreview() }}
                className="p-1 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors"><Play className="w-3.5 h-3.5" /></button></Tooltip>
          }
          <Tooltip text={pinned ? "取消固定" : "固定面板"}>
            <button onClick={e => { e.stopPropagation(); onTogglePin() }}
              className={`p-1 rounded-md transition-colors ${pinned ? 'text-accent hover:bg-bg-tertiary' : 'text-text-secondary hover:text-text-primary hover:bg-bg-tertiary'}`}>
              <Pin className={`w-3.5 h-3.5 ${pinned ? 'fill-current' : ''}`} />
            </button>
          </Tooltip>
          <ChevronDown className={`w-4 h-4 text-text-muted transition-transform duration-150 ${expanded?'rotate-180':''}`} />
        </div>
      </div>
      <div className="grid transition-[grid-template-rows] duration-150 ease-out"
        style={{ gridTemplateRows: expanded ? '1fr' : '0fr' }}>
        <div className="overflow-hidden min-h-0" data-layout-measure="">
          <div className="border-t border-border" />
          <div className="p-3">
            <div className="w-full rounded-lg bg-bg-primary overflow-hidden flex items-center justify-center"
              style={{ aspectRatio: screenRatio }}>
              <canvas ref={canvasRef}
                className={`max-w-full max-h-full object-contain ${hasContent ? '' : 'hidden'}`}
                style={{ aspectRatio: canvasDims.w && canvasDims.h ? `${canvasDims.w}/${canvasDims.h}` : '16/9' }} />
              {!hasContent && <span className="text-sm text-text-muted">点击 📷 单帧截图 或 ▶ 实时预览</span>}
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}


// ═══ LogManager — single source of truth for all log views ═══
// All logs (C++ LOG + TS addLog) flow through C++ ring buffer + log file.
// TS addLog sends to C++ via log_ui_event → C++ writes to ring buffer + file.
// LogManager periodically syncs from C++ ring buffer so all three views
// (right panel, Log tab, log files) show identical content.
type LogEntry = { ts: string; msg: string }
function timeStr() { const d = new Date(); return `${d.getHours().toString().padStart(2,'0')}:${d.getMinutes().toString().padStart(2,'0')}:${d.getSeconds().toString().padStart(2,'0')}.${d.getMilliseconds().toString().padStart(3,'0')}` }

class LogManager {
  private entries: LogEntry[] = []
  private listeners = new Set<() => void>()
  private initialSyncDone = false

  add(msg: string) {
    // Immediate local entry for instant UI feedback (matching C++ ring buffer format)
    this.entries.push({ ts: timeStr(), msg: `[ui] ${msg}` })
    this.listeners.forEach(f => f())
    // Also write to C++ log file via log_ui_event → capture_log_write_ui()
    hostCall('log_ui_event', { event: msg, detail: '' }).catch(() => {})
  }

  // Called from C++ push (capture_log_set_notify callback via WebMessage type:'log')
  addRemote(ts: string, tag: string, msg: string) {
    // Don't duplicate if we already have this exact entry
    const dup = this.entries.find(e => e.ts === ts && e.msg === `[${tag}] ${msg}`)
    if (dup) return
    this.entries.push({ ts, msg: `[${tag}] ${msg}` })
    // Cap at 500 entries
    if (this.entries.length > 500) this.entries = this.entries.slice(-500)
    this.listeners.forEach(f => f())
  }

  getAll(): LogEntry[] { return this.entries }

  subscribe(fn: () => void): () => void {
    this.listeners.add(fn)
    return () => { this.listeners.delete(fn) }
  }

  // One-time sync from C++ ring buffer at startup (catches entries before WebView2 was ready)
  async initSync() {
    if (this.initialSyncDone) return
    this.initialSyncDone = true
    try {
      const res = await hostCall('read_live_log')
      const raw = (typeof res === 'string') ? res : (res?.lines || '')
      if (!raw) return
      const lines = raw.split('\n').filter((l: string) => l.trim())
      for (const line of lines) {
        const m = line.match(/^\[(\d{2}:\d{2}:\d{2}\.\d{3})\]\s\[(\w+)\]\s(.+)$/)
        if (m) {
          this.entries.push({ ts: m[1], msg: `[${m[2]}] ${m[3]}` })
        }
      }
      // Sort + cap
      this.entries.sort((a, b) => a.ts.localeCompare(b.ts))
      if (this.entries.length > 500) this.entries = this.entries.slice(-500)
      this.listeners.forEach(f => f())
    } catch (_) {}
  }

  clear() {
    this.entries = []
    this.listeners.forEach(f => f())
    this.initialSyncDone = false  // allow re-sync after C++ ring buffer reset
    hostCall('clear_log').catch(() => {})
    // Re-sync C++ ring buffer (header + initial LOG entries)
    setTimeout(() => this.initSync(), 100)
  }

  async loadHistory(maxFiles: number): Promise<HistoryFile[]> {
    try {
      const data = await hostCall('read_logs', { max_files: maxFiles })
      const payload = data?.result || data
      const files = payload?.files || []
      return files.map((f: any) => ({ name: f.name, lines: [] as string[] }))
    } catch (e) {
      return []
    }
  }
}
const logMgr = new LogManager()

function addLog(msg: string) { logMgr.add(msg) }

// ═══ Log Panel ───
type HistoryFile = { name: string; lines: string[] }

function LogPanel({ compact, expanded: exp, onToggle, keepFiles, pinned, onTogglePin }: { compact?: boolean; expanded?: boolean; onToggle?: () => void; keepFiles?: number; pinned?: boolean; onTogglePin?: () => void }) {
  const [localExpanded, setLocalExpanded] = useState(true)
  const expanded = exp !== undefined ? exp : localExpanded
  const toggle = onToggle || (() => setLocalExpanded(v => !v))
  const scrollRef = useRef<HTMLDivElement>(null)
  const sessionScrollRef = useRef<HTMLDivElement>(null)
  const [scrolledUp, setScrolledUp] = useState(false)
  const [cardsScrolledUp, setCardsScrolledUp] = useState<Set<number>>(new Set())
  const cardScrollRefs = useRef<Map<number, HTMLDivElement>>(new Map())
  const [historyFiles, setHistoryFiles] = useState<HistoryFile[]>([])
  const [openFiles, setOpenFiles] = useState<Set<number>>(new Set())
  const [currentExpanded, setCurrentExpanded] = useState(true)
  const [sessionCopied, setSessionCopied] = useState(false)
  const [copiedFileIdx, setCopiedFileIdx] = useState<number | null>(null)
  const [refreshingIdx, setRefreshingIdx] = useState<number | null>(null)
  const [entries, setEntries] = useState(logMgr.getAll())

  // Subscribe to LogManager for live updates
  useEffect(() => {
    setEntries(logMgr.getAll())
    return logMgr.subscribe(() => setEntries([...logMgr.getAll()]))
  }, [])

  // Load history file names on mount (full mode only — compact sidebar doesn't show them)
  useEffect(() => {
    if (compact) return
    logMgr.loadHistory(keepFiles ?? 5).then(setHistoryFiles)
  }, [keepFiles, compact])

  // Format in-memory entries as display lines (same format as disk)
  const formatLine = (e: LogEntry) => `[${e.ts}] ${e.msg}`
  const currentLines = entries.map(formatLine)
  const displayLines = compact ? currentLines.slice(-100) : currentLines.slice(-500)

  // Auto-scroll: only if user hasn't scrolled up manually
  useEffect(() => {
    const ref = compact ? scrollRef.current : sessionScrollRef.current
    if (!ref) return
    const onScroll = () => {
      const atBottom = ref.scrollTop + ref.clientHeight >= ref.scrollHeight - 40
      setScrolledUp(!atBottom)
    }
    ref.addEventListener('scroll', onScroll, { passive: true })
    return () => ref.removeEventListener('scroll', onScroll)
  }, [compact])

  const entryCount = entries.length
  useEffect(() => {
    const ref = compact ? scrollRef.current : sessionScrollRef.current
    if (!ref || scrolledUp) return
    requestAnimationFrame(() => { ref.scrollTop = ref.scrollHeight })
  }, [entryCount, compact, scrolledUp])

  // Full-card mode (Log tab): current session + history cards
  if (!compact) {
    return (
      <div className="flex-1 min-h-0 overflow-y-auto p-6 space-y-3">
        {/* Current session card */}
        <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
          <div role="button" tabIndex={0}
            onClick={() => { setCurrentExpanded(v => !v); addLog(`[Log] Current Session ${currentExpanded ? 'collapsed' : 'expanded'}`) }}
            onKeyDown={e=>{if(e.key==='Enter'||e.key===' '){(e.currentTarget as HTMLElement).click()}}}
            className={COLLAPSIBLE_HEADER}>
            <div className="flex items-center gap-2">
              <FileText className="w-4 h-4 text-accent shrink-0" />
              <span className="text-sm font-medium text-text-primary">Current Session</span>
              <span className="text-xs text-text-muted">({displayLines.length})</span>
            </div>
            <div className="flex items-center gap-0.5">
              <Tooltip text={scrolledUp ? "滚动到底部" : "已在底部"}>
                <button onClick={e => { e.stopPropagation(); sessionScrollRef.current?.scrollTo({top: sessionScrollRef.current.scrollHeight, behavior: 'smooth'}) }}
                  disabled={!scrolledUp}
                  className={`p-1 rounded-md transition-colors ${scrolledUp ? 'text-accent bg-accent/15 hover:bg-accent/25' : 'text-text-muted/30 cursor-not-allowed'}`}>
                  <ArrowDown className="w-3.5 h-3.5" />
                </button>
              </Tooltip>
              <Tooltip text="复制全部日志"><button onClick={e => { e.stopPropagation(); navigator.clipboard.writeText(displayLines.join('\n')); setSessionCopied(true); setTimeout(() => setSessionCopied(false), 1500) }}
                className="p-1 rounded-md text-text-secondary hover:text-accent hover:bg-bg-tertiary transition-colors">{sessionCopied ? <Check className="w-3.5 h-3.5 text-green-400" /> : <Copy className="w-3.5 h-3.5" />}</button></Tooltip>
              <ChevronDown className={`w-4 h-4 text-text-muted transition-transform duration-150 shrink-0 ${currentExpanded?'rotate-180':''}`} />
            </div>
          </div>
          <div className="grid transition-[grid-template-rows] duration-150 ease-out"
            style={{ gridTemplateRows: currentExpanded ? '1fr' : '0fr' }}>
            <div className="overflow-hidden min-h-0">
              <div className="border-t border-border" />
              <div ref={sessionScrollRef} className="h-[400px] overflow-y-auto p-4 font-mono text-xs">
                <div className="min-h-full flex flex-col justify-end space-y-0.5">
                {displayLines.length === 0
                  ? <div className="text-text-muted text-center py-4">No logs yet</div>
                  : displayLines.map((l, i) => {
                      const last = i === displayLines.length - 1
                      const zebra = !last ? (i % 2 === 0 ? 'bg-white/[0.03]' : 'bg-black/[0.03]') : ''
                      return (
                      <div key={`cur-${i}`} className={`whitespace-pre-wrap break-all ${last ? 'font-semibold text-text-primary' : 'text-text-muted ' + zebra}`} style={{paddingLeft:'16ch',textIndent:'-16ch'}}>{l}</div>
                    )})
                }
                </div>
              </div>
            </div>
          </div>
        </div>

        {/* History file cards */}
        {historyFiles.map((f, fi) => {
          const open = openFiles.has(fi)
          return (
            <div key={f.name} className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
              <div role="button" tabIndex={0} onClick={() => {
                  const s = new Set(openFiles);
                  if (open) { s.delete(fi); }
                  else {
                    s.add(fi);
                    if (f.lines.length === 0) {
                      hostCall('read_log_file', { filename: f.name }).then(res => {
                        const content = res?.content || '';
                        const newLines = content ? content.split('\n') : [] as string[];
                        setHistoryFiles(prev => prev.map((hf, i) => i === fi ? { ...hf, lines: newLines } : hf));
                      }).catch(() => {
                        setHistoryFiles(prev => prev.map((hf, i) => i === fi ? { ...hf, lines: ['(failed to load)'] } : hf));
                      });
                    }
                  }
                  setOpenFiles(s)
                }}
                onKeyDown={e=>{if(e.key==='Enter'||e.key===' '){(e.currentTarget as HTMLElement).click()}}}
                className={COLLAPSIBLE_HEADER}>
                <div className="flex items-center gap-2 min-w-0">
                  <FileText className="w-4 h-4 text-text-muted shrink-0" />
                  <span className="text-sm font-medium text-text-primary truncate">{f.name}</span>
                  <span className="text-xs text-text-muted shrink-0">{f.lines.length > 0 ? `${f.lines.length} lines` : 'click to load'}</span>
                </div>
                <div className="flex items-center gap-0.5">
                  <Tooltip text="刷新文件内容"><button onClick={e => { e.stopPropagation(); setRefreshingIdx(fi); hostCall('read_log_file', { filename: f.name }).then(res => { const content = res?.content || ''; const newLines = content ? content.split('\n') : [] as string[]; setHistoryFiles(prev => prev.map((hf, i) => i === fi ? { ...hf, lines: newLines } : hf)); }).catch(() => {}).finally(() => setRefreshingIdx(null)) }}
                    className={`p-1 rounded-md text-text-secondary hover:text-accent hover:bg-bg-tertiary transition-colors ${refreshingIdx === fi ? 'animate-spin' : ''}`}><RefreshCw className="w-3.5 h-3.5" /></button></Tooltip>
                  <Tooltip text={cardsScrolledUp.has(fi) ? "滚动到底部" : "已在底部"}>
                    <button onClick={e => { e.stopPropagation(); const el = cardScrollRefs.current.get(fi); if (el) el.scrollTo({top: el.scrollHeight, behavior: 'smooth'}) }}
                      disabled={!cardsScrolledUp.has(fi)}
                      className={`p-1 rounded-md transition-colors ${cardsScrolledUp.has(fi) ? 'text-accent bg-accent/15 hover:bg-accent/25' : 'text-text-muted/30 cursor-not-allowed'}`}>
                      <ArrowDown className="w-3.5 h-3.5" />
                    </button>
                  </Tooltip>
                  <Tooltip text="复制文件内容"><button onClick={e => { e.stopPropagation(); navigator.clipboard.writeText(f.lines.join('\n')); setCopiedFileIdx(fi); setTimeout(() => setCopiedFileIdx(null), 1500) }}
                    className="p-1 rounded-md text-text-secondary hover:text-accent hover:bg-bg-tertiary transition-colors">{copiedFileIdx === fi ? <Check className="w-3.5 h-3.5 text-green-400" /> : <Copy className="w-3.5 h-3.5" />}</button></Tooltip>
                  <ChevronDown className={`w-4 h-4 text-text-muted transition-transform duration-150 shrink-0 ${open?'rotate-180':''}`} />
                </div>
              </div>
              <div className="grid transition-[grid-template-rows] duration-150 ease-out"
                style={{ gridTemplateRows: open ? '1fr' : '0fr' }}>
                <div className="overflow-hidden min-h-0">
                  <div className="border-t border-border" />
                  <div ref={el => { if (el) cardScrollRefs.current.set(fi, el); else cardScrollRefs.current.delete(fi) }}
                    onScroll={e => { const t = e.currentTarget; const atBottom = t.scrollTop + t.clientHeight >= t.scrollHeight - 40; setCardsScrolledUp(prev => { const s = new Set(prev); if (!atBottom) s.add(fi); else s.delete(fi); return s }) }}
                    className="h-[400px] overflow-y-auto p-4 font-mono text-xs">
                    <div className="min-h-full flex flex-col justify-end space-y-0.5">
                    {f.lines.length === 0
                      ? <div className="text-text-muted text-center py-4">Loading...</div>
                      : f.lines.map((l, i) => {
                          const last = i === f.lines.length - 1
                          const zebra = !last ? (i % 2 === 0 ? 'bg-white/[0.03]' : 'bg-black/[0.03]') : ''
                          return (
                          <div key={i} className={`whitespace-pre-wrap break-all ${last ? 'font-semibold text-text-primary' : 'text-text-muted ' + zebra}`} style={{paddingLeft:'16ch',textIndent:'-16ch'}}>{l}</div>
                        )})
                    }
                    </div>
                  </div>
                </div>
              </div>
            </div>
          )
        })}
        {historyFiles.length === 0 && (
          <div className="text-center py-6 text-xs text-text-muted">
            {entries.length === 0 ? 'No logs yet' : 'No history files found'}
          </div>
        )}
      </div>
    )
  }

  // Compact mode (right sidebar): same in-memory log content as Current Session
  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden flex flex-col min-h-0">
      <div role="button" tabIndex={0} onClick={() => { toggle(); addLog(`[Log] ${!expanded ? 'expanded' : 'collapsed'}`) }} onKeyDown={e=>{if(e.key==='Enter'||e.key===' '){(e.currentTarget as HTMLElement).click()}}}
        className={`${COLLAPSIBLE_HEADER} shrink-0`}>
        <div className="flex items-center gap-2">
          <FileText className="w-4 h-4 text-text-secondary" />
          <span className="text-sm font-medium text-text-primary">Log</span>
          <span className="text-xs text-text-muted">({displayLines.length})</span>
        </div>
        <div className="flex items-center gap-0.5">
          <Tooltip text={scrolledUp ? "滚动到底部" : "已在底部"}>
            <button onClick={e => { e.stopPropagation(); scrollRef.current?.scrollTo({top: scrollRef.current.scrollHeight, behavior: 'smooth'}) }}
              disabled={!scrolledUp}
              className={`p-1 rounded-md transition-colors ${scrolledUp ? 'text-accent bg-accent/15 hover:bg-accent/25' : 'text-text-muted/30 cursor-not-allowed'}`}>
              <ArrowDown className="w-3.5 h-3.5" />
            </button>
          </Tooltip>
          <Tooltip text="复制日志">
            <button onClick={e => { e.stopPropagation(); navigator.clipboard.writeText(displayLines.join('\n')); setSessionCopied(true); setTimeout(() => setSessionCopied(false), 1500) }}
              className="p-1 rounded-md text-text-secondary hover:text-accent hover:bg-bg-tertiary transition-colors">
              {sessionCopied ? <Check className="w-3.5 h-3.5 text-green-400" /> : <Copy className="w-3.5 h-3.5" />}
            </button>
          </Tooltip>
          {pinned !== undefined && onTogglePin && (
            <Tooltip text={pinned ? "取消固定" : "固定面板"}>
              <button onClick={e => { e.stopPropagation(); onTogglePin() }}
                className={`p-1 rounded-md transition-colors ${pinned ? 'text-accent hover:bg-bg-tertiary' : 'text-text-secondary hover:text-text-primary hover:bg-bg-tertiary'}`}>
                <Pin className={`w-3.5 h-3.5 ${pinned ? 'fill-current' : ''}`} />
              </button>
            </Tooltip>
          )}
          <ChevronDown className={`w-4 h-4 text-text-muted transition-transform duration-150 ${expanded?'rotate-180':''}`} />
        </div>
      </div>
      <div className="grid transition-[grid-template-rows] duration-150 ease-out flex-1 min-h-0"
        style={{ gridTemplateRows: expanded ? '1fr' : '0fr' }}>
        <div className="overflow-hidden min-h-0" data-layout-measure="">
          <div className="border-t border-border" />
          <div ref={scrollRef} className="h-[180px] overflow-y-auto p-4">
            {displayLines.length === 0 ? (
              <div className="flex items-center justify-center h-full text-sm text-text-muted">No logs</div>
            ) : (
              <div className="space-y-1 font-mono text-xs text-text-muted pt-1">
                {displayLines.slice(-100).map((l, i, arr) => {
                  const last = i === arr.length - 1
                  const zebra = !last ? (i % 2 === 0 ? 'bg-white/[0.03]' : 'bg-black/[0.03]') : ''
                  return (
                  <div key={`c-${i}`} className={last ? 'font-semibold text-text-primary' : 'text-text-muted ' + zebra}>{l}</div>
                )})}
              </div>
            )}
          </div>
        </div>
      </div>
    </div>
  )
}

// ═══ Settings Card (MXU-style collapsible) ═══
function SettingsCard({ icon, title, defaultExpanded, children }: {
  icon: React.ReactNode; title: string; defaultExpanded?: boolean; children: React.ReactNode
}) {
  const [expanded, setExpanded] = useState(defaultExpanded ?? true)
  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
      <div role="button" tabIndex={0} onClick={() => { setExpanded(!expanded); addLog(`[Settings] ${title} ${!expanded ? 'expanded' : 'collapsed'}`) }} onKeyDown={e=>{if(e.key==='Enter'||e.key===' '){(e.currentTarget as HTMLElement).click()}}}
        className={COLLAPSIBLE_HEADER}>
        <div className="flex items-center gap-2">
          {icon}
          <span className="text-sm font-medium text-text-primary">{title}</span>
        </div>
        <ChevronDown className={`w-4 h-4 text-text-muted transition-transform duration-150 ${expanded?'rotate-180':''}`} />
      </div>
      <div className="grid transition-[grid-template-rows] duration-150 ease-out"
        style={{ gridTemplateRows: expanded ? '1fr' : '0fr' }}>
        <div className="overflow-hidden min-h-0">
          <div className="border-t border-border" />
          <div className="p-4">{children}</div>
        </div>
      </div>
    </div>
  )
}

// ═══ Settings Page (unified Dashboard + Settings) ═══

// ── Status Bar (compact read-only metrics) ──
function StatusBar({ screen, appVersion }: { screen: string; appVersion: string }) {
  return (
    <div className="flex items-center gap-4 px-4 py-2.5 bg-bg-secondary rounded-xl ring-1 ring-inset ring-border text-xs text-text-secondary">
      <span className="flex items-center gap-1.5"><Monitor className="w-3.5 h-3.5" />{screen}</span>
      <span className="text-border">|</span>
      <span className="flex items-center gap-1.5"><RefreshCw className="w-3.5 h-3.5" />{appVersion}</span>
      <span className="text-border">|</span>
      <span className="flex items-center gap-1.5"><span className="w-2 h-2 rounded-full bg-success" />Ready</span>
      <span className="flex-1" />
      <span className="text-text-muted hidden sm:inline">Game Agent Monitor</span>
    </div>
  )
}

function SettingsView({ snapMethod, setSnapMethod, streamMethod, setStreamMethod, renderMethod, setRenderMethod, autoSnap, setAutoSnap, autoStream, setAutoStream, selWin, winState, expectedCaptureState, setExpectedCaptureState, onSelect, onDisconnect, keepFiles, setKeepFiles, appVersion, theme, setTheme, devMode, setDevMode, saveCaptureFrames, setSaveCaptureFrames, saveStreamFrames, setSaveStreamFrames, frameDumpDir, setFrameDumpDir }: { snapMethod: string; setSnapMethod: (m: string) => void; streamMethod: string; setStreamMethod: (m: string) => void; renderMethod: string; setRenderMethod: (m: string) => void; autoSnap?: boolean; setAutoSnap?: (v: boolean) => void; autoStream?: boolean; setAutoStream?: (v: boolean) => void; selWin?: WindowInfo; winState: string; expectedCaptureState?: string; setExpectedCaptureState?: (s: string) => void; onSelect: (w: WindowInfo) => void; onDisconnect: () => void; keepFiles: number; setKeepFiles: (n: number) => void; appVersion: string; theme: string; setTheme: (t: 'light'|'dark'|'system') => void; devMode: boolean; setDevMode: (v: boolean) => void; saveCaptureFrames: boolean; setSaveCaptureFrames: (v: boolean) => void; saveStreamFrames: boolean; setSaveStreamFrames: (v: boolean) => void; frameDumpDir: string; setFrameDumpDir: (d: string) => void }) {
  const colors = ['#3B82F6','#8B5CF6','#EC4899','#F59E0B','#10B981','#EF4444']
  const [accent, setAccent] = useState('#3B82F6')
  const [screenRes, setScreenRes] = useState('?×?')
  const [logDir, setLogDir] = useState('...')
  const [connExpanded, setConnExpanded] = useState(true)

  useEffect(() => {
    hostCall('screen_info').then((si: any) => setScreenRes(`${si.w}×${si.h}`)).catch(() => {})
  }, [])
  useEffect(() => {
    hostCall('get_log_dir').then((res: any) => { if (res?.dir) setLogDir(res.dir) }).catch(() => {})
  }, [])

  return (
    <div className="flex-1 overflow-y-auto p-6 space-y-3">
      <StatusBar screen={screenRes} appVersion={appVersion} />

      <ConnectionPanel onSelect={onSelect} onDisconnect={onDisconnect} snapMethod={snapMethod} setSnapMethod={setSnapMethod} streamMethod={streamMethod} setStreamMethod={setStreamMethod} selWin={selWin} winState={winState} expectedCaptureState={expectedCaptureState} setExpectedCaptureState={setExpectedCaptureState} expanded={connExpanded} onToggle={() => setConnExpanded(v => !v)} />

      <SettingsCard icon={<Camera className="w-4 h-4 text-text-secondary" />} title="Capture">
        <div className="space-y-3">
          {/* ── Snapshot + Stream side by side ── */}
          <div className="flex gap-0">
            <div className="flex-1 space-y-2 pr-3">
              <div className="flex items-center justify-between">
                <span className="text-xs text-text-muted inline-flex items-center gap-1"><Camera className="w-3.5 h-3.5" /> Snapshot</span>
                <label className="flex items-center gap-1.5 cursor-pointer select-none">
                  <span className="text-xs text-text-muted">Auto</span>
                  <button onClick={e => { e.stopPropagation(); const next = !autoSnap; setAutoSnap?.(next); if (next && selWin) { const isDesktop = selWin.hwnd === 0; setSnapMethod(isDesktop || winState === 'minimized' ? 'dxgi' : 'wgc'); } addLog(`[Setting] auto snap = ${next}`) }}
                    className={`relative w-10 h-5 rounded-full transition-colors ${autoSnap ? 'bg-amber-500' : 'bg-bg-tertiary'}`}>
                    <span className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${autoSnap ? 'translate-x-5' : ''}`} />
                  </button>
                </label>
              </div>
              <div className="flex flex-col gap-2">{CAPTURE_METHODS.map(m => {
                const isActive = snapMethod === m.v
                const ringClass = !autoSnap && isActive ? 'border-accent bg-accent/10 cursor-pointer'
                  : autoSnap && isActive ? 'border-amber-500 bg-amber-500/10 cursor-not-allowed'
                  : autoSnap ? 'border-border bg-bg-primary opacity-50 cursor-not-allowed'
                  : 'border-border bg-bg-primary hover:bg-bg-hover cursor-pointer'
                return <Tooltip key={m.v} text={m.desc}><label className={`${SELECTABLE_BTN} ${ringClass}`}><input type="radio" name="snapMethod" value={m.v} checked={isActive} disabled={autoSnap} onChange={e => { if (!autoSnap) { setSnapMethod(e.target.value); addLog(`[Setting] snap method = ${e.target.value}`) } }} className="sr-only" /><span className="text-xs font-medium text-text-primary">{m.name} <span className="text-text-muted">({m.eng})</span></span><span className="ml-auto flex items-center gap-1">{m.rec.split('/').map((t: string) => <span key={t} className="text-[11px] font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded">{t}</span>)}</span></label></Tooltip>
              })}</div>
            </div>
            <div className="w-px bg-border shrink-0" />
            <div className="flex-1 space-y-2 pl-3">
              <div className="flex items-center justify-between">
                <span className="text-xs text-text-muted inline-flex items-center gap-1"><Play className="w-3.5 h-3.5" /> Stream</span>
                <label className="flex items-center gap-1.5 cursor-pointer select-none">
                  <span className="text-xs text-text-muted">Auto</span>
                  <button onClick={e => { e.stopPropagation(); const next = !autoStream; setAutoStream?.(next); if (next) setStreamMethod('wgc'); addLog(`[Setting] auto stream = ${next}`) }}
                    className={`relative w-10 h-5 rounded-full transition-colors ${autoStream ? 'bg-amber-500' : 'bg-bg-tertiary'}`}>
                    <span className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${autoStream ? 'translate-x-5' : ''}`} />
                  </button>
                </label>
              </div>
              <div className="flex flex-col gap-2">{CAPTURE_METHODS.map(m => {
                const unsupported = m.v === 'dxgi'
                const isActive = streamMethod === m.v
                const ringClass = !autoStream && isActive && !unsupported ? 'border-accent bg-accent/10 cursor-pointer'
                  : autoStream && isActive ? 'border-amber-500 bg-amber-500/10 cursor-not-allowed'
                  : autoStream || unsupported ? 'border-border bg-bg-primary opacity-50 cursor-not-allowed'
                  : 'border-border bg-bg-primary hover:bg-bg-hover cursor-pointer'
                return <Tooltip key={m.v} text={unsupported ? 'DXGI 流未实现，仅 WGC 支持实时预览' : m.desc}><label className={`${SELECTABLE_BTN} ${ringClass}`}><input type="radio" name="streamMethod" value={m.v} checked={isActive} disabled={autoStream || unsupported} onChange={e => { if (!autoStream && !unsupported) { setStreamMethod(e.target.value); addLog(`[Setting] stream method = ${e.target.value}`) } }} className="sr-only" /><span className="text-xs font-medium text-text-primary">{m.name} <span className="text-text-muted">({m.eng})</span></span><span className="ml-auto flex items-center gap-1">{unsupported ? <span className="text-[11px] font-medium text-text-muted bg-bg-tertiary px-1.5 py-0.5 rounded">未实现</span> : m.rec.split('/').map((t: string) => <span key={t} className="text-[11px] font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded">{t}</span>)}</span></label></Tooltip>
              })}</div>
            </div>
          </div>
          {/* ── Render Method ── */}
          <div className="border-t border-border pt-3 space-y-2">
            <div className="flex items-center justify-between">
              <span className="text-xs text-text-muted">Render Method (🎨)</span>
            </div>
            <div className="flex flex-col gap-2">{RENDER_METHODS.map(m => {
              const isActive = renderMethod === m.v
              const implemented = m.v === 'shared'
              const ringClass = isActive && implemented ? 'border-accent bg-accent/10 cursor-pointer'
                : implemented ? 'border-border bg-bg-primary hover:bg-bg-hover cursor-pointer'
                : 'border-border bg-bg-primary opacity-50 cursor-not-allowed'
              return <Tooltip key={m.v} text={m.desc}><label className={`${SELECTABLE_BTN} ${ringClass}`}><input type="radio" name="renderMethod" value={m.v} checked={isActive} disabled={!implemented} onChange={e => { if (implemented) { setRenderMethod(e.target.value); addLog(`[Setting] render method = ${e.target.value}`) } }} className="sr-only" /><span className="text-xs font-medium text-text-primary">{m.name} <span className="text-text-muted">({m.eng})</span></span><span className="ml-auto flex items-center gap-1">{m.rec.split('/').map((t: string) => <span key={t} className={`text-[11px] font-medium px-1.5 py-0.5 rounded ${implemented ? 'text-accent bg-accent/10' : 'text-text-muted bg-bg-tertiary'}`}>{t}</span>)}</span></label></Tooltip>
            })}</div>
          </div>
        </div>
      </SettingsCard>

      <SettingsCard icon={<Cpu className="w-4 h-4 text-text-secondary" />} title="Model">
        <div className="text-xs text-text-muted mb-2">Base model + fine-tuning adapter for game-specific AI.</div>
        <div className="flex items-center gap-3 mb-2"><label className="text-sm text-text-secondary w-24 shrink-0">Model</label><Tooltip text="基础视觉模型" className="flex-1 min-w-0"><input defaultValue="GenericAgent v1" onBlur={e => addLog(`[Setting] base model = ${e.target.value}`)} className="w-full h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
        <div className="flex items-center gap-3"><label className="text-sm text-text-secondary w-24 shrink-0">Adapter</label><Tooltip text="游戏微调权重" className="flex-1 min-w-0"><input defaultValue="tictactoe-finetune" onBlur={e => addLog(`[Setting] adapter = ${e.target.value}`)} className="w-full h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
      </SettingsCard>

      <SettingsCard icon={<Sun className="w-4 h-4 text-text-secondary" />} title="General">
        <div className="space-y-3">
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">Theme</label>
            <div className="flex gap-1">
              {[['Light','light'],['Dark','dark'],['System','system']].map(([l,v])=>
                <button key={v} onClick={()=>{setTheme(v as 'light'|'dark'|'system'); addLog(`[Theme] ${l}`)}}
                  className={`px-3 py-1 rounded-full text-xs font-medium transition-colors ${theme === v ? 'bg-accent text-white' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'}`}>{l}</button>
              )}
            </div>
          </div>
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">Accent</label>
            <div className="flex gap-1.5">
              {colors.map(c=>(
                <button key={c} onClick={()=>{
                  setAccent(c);
                  document.documentElement.style.setProperty('--color-accent', c);
                  addLog(`[Theme] accent = ${c}`)
                }}
                  className="w-6 h-6 rounded-full transition-all duration-150"
                  style={{
                    background: c,
                    boxShadow: accent===c
                      ? `0 0 0 2px var(--color-bg-secondary), 0 0 0 4px ${c}`
                      : 'none'
                  }} />
              ))}
            </div>
          </div>
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">Log dir</label>
            <Tooltip text="日志文件存放路径" className="flex-1 min-w-0"><input value={logDir} readOnly className="w-full h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm text-text-muted outline-none cursor-default font-mono text-xs truncate" /></Tooltip>
            <Tooltip text="修改日志目录"><button onClick={async () => { try { const res = await hostCall('pick_log_dir'); if (res?.dir) { setLogDir(res.dir); addLog(`[Setting] log dir = ${res.dir}`) } } catch(_) {} }} className="shrink-0 p-1.5 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors"><Pencil className="w-4 h-4" /></button></Tooltip>
            <Tooltip text="在资源管理器中打开日志目录"><button onClick={() => hostCall('open_log_dir').catch(() => {})} className="shrink-0 p-1.5 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors"><FolderOpen className="w-4 h-4" /></button></Tooltip>
          </div>
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">Keep files</label>
            <Tooltip text="Log 菜单中显示的历史日志文件数"><select value={keepFiles} onChange={e => setKeepFiles(Number(e.target.value))} className="h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent">{[3,5,7,10].map(n=><option key={n} value={n}>{n} files</option>)}</select></Tooltip>
          </div>
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">Dev mode</label>
            <button onClick={() => { setDevMode(!devMode); addLog(`[Dev] ${!devMode ? 'ON' : 'OFF'}`) }}
              className={`relative w-10 h-5 rounded-full transition-colors ${devMode ? 'bg-amber-500' : 'bg-bg-tertiary'}`}>
              <span className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${devMode ? 'translate-x-5' : ''}`} />
            </button>
          </div>
        </div>
      </SettingsCard>

      {devMode && <SettingsCard icon={<Cpu className="w-4 h-4 text-amber-400" />} title="Developer Mode" defaultExpanded={true}>
        <div className="space-y-3">
          <div className="flex items-center justify-between">
            <div><div className="text-sm text-text-primary">Save single-frame captures</div><div className="text-xs text-text-muted">Save each 📷 snapshot as PNG to disk</div></div>
            <button onClick={() => { const v = !saveCaptureFrames; setSaveCaptureFrames(v); hostCall('set_frame_dump', { capture: v, stream: saveStreamFrames, dir: frameDumpDir }).catch(() => {}) }}
              className={`relative w-10 h-5 rounded-full transition-colors ${saveCaptureFrames ? 'bg-success' : 'bg-bg-tertiary'}`}>
              <span className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${saveCaptureFrames ? 'translate-x-5' : ''}`} />
            </button>
          </div>
          <div className="flex items-center justify-between">
            <div><div className="text-sm text-text-primary">Save live preview frames</div><div className="text-xs text-text-muted">Save each ▶ preview frame as PNG to disk</div></div>
            <button onClick={() => { const v = !saveStreamFrames; setSaveStreamFrames(v); hostCall('set_frame_dump', { capture: saveCaptureFrames, stream: v, dir: frameDumpDir }).catch(() => {}) }}
              className={`relative w-10 h-5 rounded-full transition-colors ${saveStreamFrames ? 'bg-success' : 'bg-bg-tertiary'}`}>
              <span className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${saveStreamFrames ? 'translate-x-5' : ''}`} />
            </button>
          </div>
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">Dump dir</label>
            <Tooltip text="帧保存路径" className="flex-1 min-w-0"><input value={frameDumpDir || '(not set)'} readOnly className="w-full h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm text-text-muted outline-none cursor-default font-mono text-xs truncate" /></Tooltip>
            <Tooltip text="选择保存目录"><button onClick={async () => { try { const res = await hostCall('pick_dir'); if (res?.dir) { setFrameDumpDir(res.dir); if (!saveCaptureFrames) setSaveCaptureFrames(true); if (!saveStreamFrames) setSaveStreamFrames(true); hostCall('set_frame_dump', { capture: true, stream: true, dir: res.dir }).catch(() => {}); addLog(`[Dev] dump dir = ${res.dir}`) } } catch(_) {} }} className="shrink-0 p-1.5 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors"><Pencil className="w-4 h-4" /></button></Tooltip>
            <Tooltip text="在资源管理器中打开保存目录"><button onClick={() => { if (frameDumpDir) hostCall('open_dir', { dir: frameDumpDir }).catch(() => {}) }} className="shrink-0 p-1.5 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors"><FolderOpen className="w-4 h-4" /></button></Tooltip>
          </div>
        </div>
      </SettingsCard>}

      <SettingsCard icon={<RefreshCw className="w-4 h-4 text-text-secondary" />} title="About">
        <div className="space-y-3">
          <div className="flex items-center justify-between">
            <div><div className="text-sm text-text-primary">Game Agent Monitor</div><div className="text-xs text-text-muted">Version {appVersion}</div></div>
            <ActionBtn icon={<RefreshCw className="w-3.5 h-3.5" />} label="Check Update" title="检查新版本" variant="outline" onClick={() => addLog('[Action] check update')} />
          </div>
          <div className="border-t border-border pt-2 flex items-center justify-between">
            <div className="text-xs text-text-muted min-w-0">
              <button onClick={()=>{try{window.open('https://github.com/Andyqwe44/tictactoe','_blank')}catch{}; addLog('[Project] open GitHub')}} className="text-accent hover:underline cursor-pointer truncate">github.com/Andyqwe44/tictactoe</button>
              <span className="mx-2 text-border hidden sm:inline">|</span>
              <span className="hidden sm:inline">C++ WebView2 · React · Tailwind · DXGI · WGC</span>
            </div>
            <Tooltip text="给项目点Star支持开发"><button onClick={()=>{try{window.open('https://github.com/Andyqwe44/tictactoe','_blank')}catch{}}}
              className="inline-flex items-center gap-1.5 rounded-md px-2.5 h-7 text-xs font-medium bg-accent text-white hover:bg-accent-hover transition-colors shrink-0 ml-2"><span>★</span>Star</button></Tooltip>
          </div>
        </div>
      </SettingsCard>

      <div className="h-4" />
    </div>
  )
}

export default function App() {
  const [tab, setTab] = useState<'Monitor'|'Log'|'Settings'>('Settings')
  const [running, setRunning] = useState(false)
  const [appVersion, setAppVersion] = useState('v0.3.0') // fallback; fetched from C++ on mount
  const [rightWidth, setRightWidth] = useState(DEFAULT_RIGHT_WIDTH)
  const [rightCollapsed, setRightCollapsed] = useState(false)
  const [connectionExpanded, setConnectionExpanded] = useState(true)
  const [screenshotExpanded, setScreenshotExpanded] = useState(false) // start collapsed; auto-expand on snapshot/stream
  const [logExpanded, setLogExpanded] = useState(true)
  const connectionExpandedRef = useRef(connectionExpanded)
  connectionExpandedRef.current = connectionExpanded
  const screenshotExpandedRef = useRef(screenshotExpanded)
  screenshotExpandedRef.current = screenshotExpanded
  const logExpandedRef = useRef(logExpanded)
  logExpandedRef.current = logExpanded
  // ── Pin lock: safe setter gate blocks all changes when locked ──
  // null = unlocked; true/false = locked at that expanded state
  const connPinLocked = useRef<boolean | null>(null)
  const ssPinLocked = useRef<boolean | null>(null)
  const logPinLocked = useRef<boolean | null>(null)
  const [connectionPinned, setConnectionPinned] = useState(false)
  const [screenshotPinned, setScreenshotPinned] = useState(false)
  const [logPinned, setLogPinned] = useState(false)
  const ssHasContentRef = useRef(false)

  // Safe setters — single gate, no scattered guard clauses needed
  const setConnExpanded = useCallback((v: boolean): boolean => {
    if (connPinLocked.current !== null) return false
    setConnectionExpanded(v)
    return true
  }, [])
  const setSsExpanded = useCallback((v: boolean): boolean => {
    if (ssPinLocked.current !== null) return false
    setScreenshotExpanded(v)
    return true
  }, [])
  const setLogPanelExpanded = useCallback((v: boolean): boolean => {
    if (logPinLocked.current !== null) return false
    setLogExpanded(v)
    return true
  }, [])

  // Pin toggle: lock at current state / unlock
  const toggleConnPin = useCallback(() => {
    if (connPinLocked.current === null) {
      connPinLocked.current = connectionExpandedRef.current
      setConnectionPinned(true)
    } else {
      connPinLocked.current = null
      setConnectionPinned(false)
    }
  }, [])
  const toggleSsPin = useCallback(() => {
    if (ssPinLocked.current === null) {
      ssPinLocked.current = screenshotExpandedRef.current
      setScreenshotPinned(true)
    } else {
      ssPinLocked.current = null
      setScreenshotPinned(false)
    }
  }, [])
  const toggleLogPin = useCallback(() => {
    if (logPinLocked.current === null) {
      logPinLocked.current = logExpandedRef.current
      setLogPinned(true)
    } else {
      logPinLocked.current = null
      setLogPinned(false)
    }
  }, [])
  const rightPanelRef = useRef<HTMLDivElement>(null)

  // ── Theme state (shared by ThemeBtn + Settings General) ──
  const [theme, setTheme] = useState<'light'|'dark'|'system'>('light')
  const [systemDark, setSystemDark] = useState(false)
  const resolvedDark = theme === 'system' ? systemDark : theme === 'dark'

  // Listen to OS color scheme changes
  useEffect(() => {
    const mq = window.matchMedia('(prefers-color-scheme: dark)')
    setSystemDark(mq.matches)
    const onChange = (e: MediaQueryListEvent) => setSystemDark(e.matches)
    mq.addEventListener('change', onChange)
    return () => mq.removeEventListener('change', onChange)
  }, [])

  // Apply theme when resolvedDark changes
  useEffect(() => {
    applyTheme(resolvedDark)
  }, [resolvedDark])

  // ── Right panel auto-layout state machine ──
  // Heights measured from DOM: C/S/L = expanded, Cp/Sp/Lp = collapsed (prime).
  const H = useRef({ C: 180, S: 300, L: 250, Cp: 44, Sp: 44, Lp: 44 })
  const GAP = 60 // p-3 (24px) + 3 × gap-3 (36px)
  const prevClientH = useRef(0)
  const guard = useRef({ C: 0, S: 0, L: 0 })

  const measureLayout = useCallback(() => {
    const el = rightPanelRef.current; if (!el) return
    const kids = el.querySelectorAll(':scope > div')
    if (kids.length < 3) return
    const gh = (i: number) => (kids[i] as HTMLElement).offsetHeight
    if (connectionExpandedRef.current) H.current.C = gh(0); else H.current.Cp = gh(0)
    if (screenshotExpandedRef.current) H.current.S = gh(1); else H.current.Sp = gh(1)
    if (logExpandedRef.current) H.current.L = gh(2); else H.current.Lp = gh(2)
    // Estimate expanded height from collapsed state via inner scrollHeight
    // Uses data-layout-measure attribute instead of CSS class string (resilient to refactoring)
    if (!connectionExpandedRef.current) {
      const inner = kids[0].querySelector('[data-layout-measure]') as HTMLElement | null
      if (inner) H.current.C = Math.max(H.current.C, gh(0) + inner.scrollHeight)
    }
    if (!screenshotExpandedRef.current) {
      const inner = kids[1].querySelector('[data-layout-measure]') as HTMLElement | null
      if (inner) H.current.S = Math.max(H.current.S, gh(1) + inner.scrollHeight)
    }
    if (!logExpandedRef.current) {
      const inner = kids[2].querySelector('[data-layout-measure]') as HTMLElement | null
      if (inner) H.current.L = Math.max(H.current.L, gh(2) + inner.scrollHeight)
    }
  }, [])

  // Re-measure after CSS grid animation (150ms) completes
  useEffect(() => {
    const t = setTimeout(measureLayout, 200)
    return () => clearTimeout(t)
  }, [connectionExpanded, screenshotExpanded, logExpanded, measureLayout])

  useEffect(() => { measureLayout() }, [])

  // Fetch canonical version from C++ backend (single source of truth)
  useEffect(() => {
    hostCall('get_version').then((v: string) => { if (v) setAppVersion(v.startsWith('v') ? v : `v${v}`) }).catch(() => {})
  }, [])

  // Sync LogManager with C++ ring buffer at startup (catch up entries before WebView2 was ready).
  // After init, C++ pushes new entries in real-time via capture_log_set_notify → type:'log' WebMessage.
  useEffect(() => {
    logMgr.initSync()
  }, [])

  // Initial layout check — resize event doesn't fire on startup. If all panels
  // expanded overflow the right panel, collapse L → C → S(empty) → S(content).
  // Safe setters reject pinned panels automatically.
  useEffect(() => {
    const check = () => {
      measureLayout()
      const el = rightPanelRef.current; if (!el) return
      const kids = el.querySelectorAll(':scope > div')
      let kidsH = 0
      for (let i = 0; i < 3; i++) kidsH += (kids[i] as HTMLElement).offsetHeight
      const overflow = kidsH + GAP - el.clientHeight
      if (overflow > 4) {
        // Priority: Log → Connection → Screenshot(empty) → Screenshot(content)
        if (logExpandedRef.current && logPinLocked.current === null) {
          addLog(`[Layout] init overflow ${overflow}px → auto-collapse log`)
          setLogExpanded(false)
          setTimeout(check, 250)
        } else if (connectionExpandedRef.current && connPinLocked.current === null) {
          addLog(`[Layout] init overflow ${overflow}px → auto-collapse connection`)
          setConnectionExpanded(false)
          setTimeout(check, 250)
        } else if (screenshotExpandedRef.current && ssPinLocked.current === null && !ssHasContentRef.current) {
          addLog(`[Layout] init overflow ${overflow}px → auto-collapse screenshot`)
          setScreenshotExpanded(false)
          setTimeout(check, 250)
        } else if (screenshotExpandedRef.current && ssPinLocked.current === null && ssHasContentRef.current) {
          addLog(`[Layout] init overflow ${overflow}px → auto-collapse screenshot (has content)`)
          setScreenshotExpanded(false)
        }
      }
      prevClientH.current = el.clientHeight
    }
    const t = setTimeout(check, 400)
    return () => clearTimeout(t)
  }, [])

  // Resize event → measure actual kidsH, compare against clientHeight
  useEffect(() => {
    const onResize = () => {
      const el = rightPanelRef.current; if (!el) return
      const ch = el.clientHeight
      if (prevClientH.current === 0) { prevClientH.current = ch; return }

      // Measure actual current total height (accounts for manual toggles)
      const kids = el.querySelectorAll(':scope > div')
      let kidsH = 0
      for (let i = 0; i < 3; i++) kidsH += (kids[i] as HTMLElement).offsetHeight
      const overflow = kidsH + GAP - ch

      const prev = prevClientH.current
      const now = Date.now()
      const h = H.current

      if (ch < prev) {
        // Shrinking: collapse L → C → S(empty) → S(content), skip pinned
        if (overflow > 4) {
          if (logExpandedRef.current && logPinLocked.current === null && now - guard.current.L > 350) {
            addLog(`[Layout] overflow ${overflow}px → auto-collapse log`)
            setLogExpanded(false); guard.current.L = now
          } else if (connectionExpandedRef.current && connPinLocked.current === null && now - guard.current.C > 350) {
            addLog(`[Layout] overflow ${overflow}px → auto-collapse connection`)
            setConnectionExpanded(false); guard.current.C = now
          } else if (screenshotExpandedRef.current && ssPinLocked.current === null && !ssHasContentRef.current && now - guard.current.S > 350) {
            addLog(`[Layout] overflow ${overflow}px → auto-collapse screenshot`)
            setScreenshotExpanded(false); guard.current.S = now
          } else if (screenshotExpandedRef.current && ssPinLocked.current === null && ssHasContentRef.current && now - guard.current.S > 350) {
            addLog(`[Layout] overflow ${overflow}px → auto-collapse screenshot (has content)`)
            setScreenshotExpanded(false); guard.current.S = now
          }
        }
      } else if (ch > prev) {
        // Growing: expand S(content) → C → S(empty) → L, skip pinned
        if (!screenshotExpandedRef.current && ssPinLocked.current === null && ssHasContentRef.current && now - guard.current.S > 350) {
          const wouldNeed = (kidsH - h.Sp + h.S) + GAP
          if (ch >= wouldNeed) {
            addLog(`[Layout] room for S (has content, need ${wouldNeed}px) → auto-expand screenshot`)
            setScreenshotExpanded(true); guard.current.S = now
          }
        } else if (!connectionExpandedRef.current && connPinLocked.current === null && now - guard.current.C > 350) {
          const wouldNeed = (kidsH - h.Cp + h.C) + GAP
          if (ch >= wouldNeed) {
            addLog(`[Layout] room for C (need ${wouldNeed}px) → auto-expand connection`)
            setConnectionExpanded(true); guard.current.C = now
          }
        } else if (!screenshotExpandedRef.current && ssPinLocked.current === null && !ssHasContentRef.current && now - guard.current.S > 350) {
          const wouldNeed = (kidsH - h.Sp + h.S) + GAP
          if (ch >= wouldNeed) {
            addLog(`[Layout] room for S (need ${wouldNeed}px) → auto-expand screenshot`)
            setScreenshotExpanded(true); guard.current.S = now
          }
        } else if (!logExpandedRef.current && logPinLocked.current === null && now - guard.current.L > 350) {
          const wouldNeed = (kidsH - h.Lp + h.L) + GAP
          if (ch >= wouldNeed) {
            addLog(`[Layout] room for L (need ${wouldNeed}px) → auto-expand log`)
            setLogExpanded(true); guard.current.L = now
          }
        }
      }

      prevClientH.current = ch
    }
    window.addEventListener('resize', onResize)
    return () => window.removeEventListener('resize', onResize)
  }, [])

  // ── Check vertical overflow after drag — auto collapse/expand panels ──
  const checkVerticalLayout = useCallback(() => {
    const el = rightPanelRef.current; if (!el) return
    const kids = el.querySelectorAll(':scope > div')
    if (kids.length < 3) return
    let kidsH = 0
    for (let i = 0; i < 3; i++) kidsH += (kids[i] as HTMLElement).offsetHeight
    const ch = el.clientHeight
    const h = H.current
    const overflow = kidsH + GAP - ch

    if (overflow > 4) {
      // Collapse L → C → S(empty) → S(content), skip pinned
      if (logExpandedRef.current && logPinLocked.current === null) {
        addLog(`[Layout] drag overflow ${overflow}px → auto-collapse log`)
        setLogExpanded(false)
      } else if (connectionExpandedRef.current && connPinLocked.current === null) {
        addLog(`[Layout] drag overflow ${overflow}px → auto-collapse connection`)
        setConnectionExpanded(false)
      } else if (screenshotExpandedRef.current && ssPinLocked.current === null && !ssHasContentRef.current) {
        addLog(`[Layout] drag overflow ${overflow}px → auto-collapse screenshot`)
        setScreenshotExpanded(false)
      } else if (screenshotExpandedRef.current && ssPinLocked.current === null && ssHasContentRef.current) {
        addLog(`[Layout] drag overflow ${overflow}px → auto-collapse screenshot (has content)`)
        setScreenshotExpanded(false)
      }
    } else if (overflow < -4) {
      // Room available — expand S(content) → C → S(empty) → L, skip pinned
      if (!screenshotExpandedRef.current && ssPinLocked.current === null && ssHasContentRef.current) {
        const wouldNeed = (kidsH - h.Sp + h.S) + GAP
        if (ch >= wouldNeed) {
          addLog(`[Layout] drag room for S (has content, need ${wouldNeed}px) → auto-expand screenshot`)
          setScreenshotExpanded(true)
        }
      } else if (!connectionExpandedRef.current && connPinLocked.current === null) {
        const wouldNeed = (kidsH - h.Cp + h.C) + GAP
        if (ch >= wouldNeed) {
          addLog(`[Layout] drag room for C (need ${wouldNeed}px) → auto-expand connection`)
          setConnectionExpanded(true)
        }
      } else if (!screenshotExpandedRef.current && ssPinLocked.current === null && !ssHasContentRef.current) {
        const wouldNeed = (kidsH - h.Sp + h.S) + GAP
        if (ch >= wouldNeed) {
          addLog(`[Layout] drag room for S (need ${wouldNeed}px) → auto-expand screenshot`)
          setScreenshotExpanded(true)
        }
      } else if (!logExpandedRef.current && logPinLocked.current === null) {
        const wouldNeed = (kidsH - h.Lp + h.L) + GAP
        if (ch >= wouldNeed) {
          addLog(`[Layout] drag room for L (need ${wouldNeed}px) → auto-expand log`)
          setLogExpanded(true)
        }
      }
    }
  }, [])

  // ── Horizontal auto-collapse: when window too narrow for left + right panels ──
  const H_COLLAPSE_THRESHOLD = MIN_LEFT_WIDTH + DEFAULT_RIGHT_WIDTH + 24 // 360+324+24=708
  const autoCollapsedByWidth = useRef(false)

  useEffect(() => {
    const onResize = () => {
      const w = window.innerWidth
      if (w < H_COLLAPSE_THRESHOLD && !rightCollapsed) {
        autoCollapsedByWidth.current = true
        setRightCollapsed(true)
        addLog('[Layout] window too narrow → auto-collapse right panel')
      } else if (w >= H_COLLAPSE_THRESHOLD && rightCollapsed && autoCollapsedByWidth.current) {
        autoCollapsedByWidth.current = false
        setRightCollapsed(false)
        addLog('[Layout] window wide enough → auto-expand right panel')
      }
    }
    window.addEventListener('resize', onResize)
    // Also check on mount
    onResize()
    return () => window.removeEventListener('resize', onResize)
  }, [rightCollapsed])

  const isResizing = useRef(false)
  const [selWindow, setSelWindow] = useState<WindowInfo>({ title: ' Entire Desktop', category: 'desktop', hwnd: 0 })
  const [screenRatio, setScreenRatio] = useState(16/9)
  const [snapMethod, setSnapMethod] = useState('dxgi')
  const [streamMethod, setStreamMethod] = useState('wgc')
  const [autoSnap, setAutoSnap] = useState(true)
  const [autoStream, setAutoStream] = useState(true)
  const [renderMethod, setRenderMethod] = useState('shared')
  const [expectedCaptureState, setExpectedCaptureState] = useState('desktop')
  const [keepFiles, setKeepFiles] = useState(5)
  const [devMode, setDevMode] = useState(false)
  const [saveCaptureFrames, setSaveCaptureFrames] = useState(false)
  const [saveStreamFrames, setSaveStreamFrames] = useState(false)
  const [frameDumpDir, setFrameDumpDir] = useState('')
  const [winState, setWinState] = useState('desktop')
  const lastWinStateRef = useRef('desktop')

  // ── Capture operation state machine (TS-side conflict resolution) ──
  const opStateRef = useRef<'idle' | 'snapshotting' | 'streaming'>('idle')
  const snapCancelRef = useRef(0)
  const [previewing, setPreviewing] = useState(false)
  const previewingRef = useRef(false)
  const snapshotRef = useRef(false)
  const snapshotStartRef = useRef(0)
  const [capMethod, setCapMethod] = useState('')

  useEffect(() => {
    (async () => {
      try {
        const si = await hostCall('screen_info')
        setScreenRatio(si.w / si.h)
      } catch (_) {}
    })()
  }, [])

  // Real-time window state polling for Connection + Screenshot panels
  useEffect(() => {
    const poll = async () => {
      try {
        const s = await hostCall('window_state', { hwnd: selWindow.hwnd })
        if (s !== lastWinStateRef.current) { lastWinStateRef.current = s; setWinState(s) }
      } catch (_) {}
    }
    poll()
    const iv = setInterval(poll, 500)
    return () => clearInterval(iv)
  }, [selWindow.hwnd])

  // Auto-select capture methods based on target and window state
  useEffect(() => {
    const isDesktop = selWindow.hwnd === 0
    if (autoSnap) {
      // Single-frame: desktop → dxgi (DesktopBlt fast), window → wgc (GPU), minimized → dxgi
      setSnapMethod(isDesktop || winState === 'minimized' ? 'dxgi' : 'wgc')
    }
    if (autoStream) {
      // Streaming: only WGC works (DXGI stream not implemented)
      setStreamMethod('wgc')
    }
  }, [selWindow.hwnd, winState, autoSnap, autoStream])

  // ── Capture operations (TS-side conflict resolution — last action wins) ──

  const stopStream = useCallback(async () => {
    if (opStateRef.current !== 'streaming') return
    previewingRef.current = false; setPreviewing(false)
    try { await hostCall('capture_stream_stop') } catch (_) {}
    opStateRef.current = 'idle'
    addLog('[Capture] stream stopped')
    // Auto-collapse ScreenshotPanel when stream stops (safe setter: no-op if pinned)
    setSsExpanded(false)
  }, [])

  const takeSnapshot = useCallback(async () => {
    // Auto-stop stream before snapshot (newer action wins)
    if (opStateRef.current === 'streaming') {
      addLog('[Capture] auto-stop stream before snapshot')
      await stopStream()
    }
    // Guard: no concurrent snapshot
    if (opStateRef.current === 'snapshotting') {
      addLog('[Capture] snapshot already in progress, ignoring')
      return
    }
    opStateRef.current = 'snapshotting'
    const snapId = ++snapCancelRef.current

    const hwnd = selWindow.hwnd ?? 0
    const method = snapMethod
    // addLog(`[Capture] 📷 snapshot start: hwnd=${hwnd} method=${method} winState=${winState}`)
    if (cantCaptureMinimized(method, winState)) {
      addLog(`[Capture] blocked: window minimized, ${method} cannot capture`)
      opStateRef.current = 'idle'
      return
    }
    // Auto-expand ScreenshotPanel to show the result (safe setter: no-op if pinned collapsed)
    if (!screenshotExpanded) setSsExpanded(true)
    // addLog(`[Capture] ${METHOD_SHORT[method] || method} ${hwnd ? 'hwnd='+hwnd : 'desktop'}...`)
    const t0 = Date.now()
    snapshotStartRef.current = t0
    snapshotRef.current = true
    try {
      const info = await hostCall('capture_window', { hwnd, method }) as { ok?: boolean; method?: string; w?: number; h?: number }
      const elapsed = Date.now() - t0
      // addLog(`[Capture] hostCall returned: ok=${info?.ok} w=${info?.w} h=${info?.h} method=${info?.method} (${elapsed}ms)`)
      // Check if cancelled by later operation (e.g. stream start)
      if (snapId !== snapCancelRef.current) {
        addLog('[Capture] snapshot cancelled by later operation')
        return
      }
      if (info && info.ok) {
        if (info.method) setCapMethod(info.method)
        addLog(`[Capture] OK ${info.w}x${info.h} (${elapsed}ms) [${info.method || '?'}]`)
      } else {
        snapshotRef.current = false
        addLog(`[Capture] failed (${elapsed}ms)`)
      }
    } catch (ex: any) {
      if (snapId !== snapCancelRef.current) return
      snapshotRef.current = false
      addLog(`[Capture] EXCEPTION: ${ex?.message || ex} after ${Date.now() - t0}ms`)
    }
    opStateRef.current = 'idle'
  }, [selWindow.hwnd, snapMethod, winState, stopStream])

  const startStream = useCallback(async () => {
    // Cancel any pending snapshot (newer action wins)
    ++snapCancelRef.current
    snapshotRef.current = false

    // Guard: stream already starting
    if (opStateRef.current === 'streaming') {
      addLog('[Capture] stream already starting, ignoring')
      return
    }

    const hwnd = selWindow.hwnd ?? 0
    if (cantCaptureMinimized(streamMethod, winState)) {
      addLog(`[Preview] blocked: window minimized, ${streamMethod} cannot capture`)
      return
    }

    opStateRef.current = 'streaming'
    previewingRef.current = true; setPreviewing(true)
    setCapMethod(streamMethod)

    // addLog(`[Preview] ${selWindow?.title ?? 'desktop'} [${streamMethod}]`)
    try {
      await hostCall('capture_stream_start', { hwnd, tcpPort: 9999, method: streamMethod, transport: renderMethod })
    } catch (e) {
      previewingRef.current = false; setPreviewing(false)
      opStateRef.current = 'idle'
      addLog(`[Preview] start failed: ${e}`)
      return
    }
    // Auto-expand ScreenshotPanel if collapsed (safe setter: no-op if pinned collapsed)
    if (!screenshotExpanded) setSsExpanded(true)
  }, [selWindow.hwnd, selWindow?.title, streamMethod, winState, renderMethod, screenshotExpanded])

  const togglePreview = useCallback(async () => {
    if (previewing) { await stopStream() }
    else { await startStream() }
  }, [previewing, stopStream, startStream])

  // Cleanup on unmount
  useEffect(() => { return () => {
    previewingRef.current = false; snapshotRef.current = false
    hostCall('capture_stream_stop').catch(() => {})
  } }, [])

  const handleResizeStart = useCallback((e: React.MouseEvent) => {
    autoCollapsedByWidth.current = false // manual drag overrides auto-collapse
    e.preventDefault(); isResizing.current = true
    document.body.style.cursor = 'col-resize'; document.body.style.userSelect = 'none'
    const onMove = (ev: MouseEvent) => {
      const w = document.body.clientWidth - ev.clientX
      if (w < 160) setRightCollapsed(true)
      else { setRightCollapsed(false); setRightWidth(Math.max(324, Math.min(400, w))) }
    }
    const onUp = () => { isResizing.current = false; document.body.style.cursor = ''; document.body.style.userSelect = ''; window.removeEventListener('mousemove', onMove); window.removeEventListener('mouseup', onUp); addLog('[Layout] right panel resized'); requestAnimationFrame(() => { measureLayout(); checkVerticalLayout() }) }
    window.addEventListener('mousemove', onMove); window.addEventListener('mouseup', onUp)
  }, [measureLayout, checkVerticalLayout])

  return (
    <div className="h-full flex flex-col bg-bg-primary">
      <TopBar tab={tab} setTab={setTab} running={running}
        onStart={() => setRunning(true)} onStop={() => setRunning(false)}
        dark={resolvedDark} onToggleTheme={() => setTheme(resolvedDark ? 'light' : 'dark')} />
      <div className="flex-1 flex overflow-hidden">
        <div className="flex-1 flex flex-col overflow-hidden border-r border-border" style={{ minWidth: MIN_LEFT_WIDTH }}>
          {tab === 'Settings' && <SettingsView snapMethod={snapMethod} setSnapMethod={setSnapMethod} streamMethod={streamMethod} setStreamMethod={setStreamMethod} renderMethod={renderMethod} setRenderMethod={setRenderMethod} autoSnap={autoSnap} setAutoSnap={setAutoSnap} autoStream={autoStream} setAutoStream={setAutoStream} selWin={selWindow} winState={winState} expectedCaptureState={expectedCaptureState} setExpectedCaptureState={setExpectedCaptureState} onSelect={(w: WindowInfo) => { if (opStateRef.current === 'streaming') stopStream(); setSelWindow(w) }} onDisconnect={() => { if (opStateRef.current === 'streaming') stopStream(); setSelWindow({ title: ' Entire Desktop', category: 'desktop', hwnd: 0 }); setExpectedCaptureState('desktop'); addLog('[Connection] disconnected, back to desktop') }} keepFiles={keepFiles} setKeepFiles={setKeepFiles} appVersion={appVersion} theme={theme} setTheme={setTheme} devMode={devMode} setDevMode={setDevMode} saveCaptureFrames={saveCaptureFrames} setSaveCaptureFrames={setSaveCaptureFrames} saveStreamFrames={saveStreamFrames} setSaveStreamFrames={setSaveStreamFrames} frameDumpDir={frameDumpDir} setFrameDumpDir={setFrameDumpDir} />}
          {tab === 'Monitor' && (
            <div className="flex-1 flex items-center justify-center p-6">
              <div className="rounded-xl bg-bg-secondary p-8 text-center space-y-3 max-w-md w-full">
                <div className="text-5xl opacity-20">🎮</div>
                <div className="text-sm text-text-secondary">{running ? 'Task running...' : 'Press Start to begin the agent'}</div>
                <div className="text-xs text-text-muted">Target: {selWindow.title}</div>
              </div>
            </div>
          )}
          {tab === 'Log' && <LogPanel keepFiles={keepFiles} />}
          <BottomBar running={running} fps={0} lat={0} />
        </div>
        <Tooltip text={rightCollapsed ? "向右拖拽展开面板" : "拖拽调整面板宽度，向右拖到底可折叠"}>
          <div onMouseDown={handleResizeStart}
            className={`${rightCollapsed ? 'w-4' : 'w-1'} hover:bg-accent/50 cursor-col-resize flex items-center justify-center group shrink-0 transition-all select-none bg-transparent`}>
            <div className="w-[2px] h-8 rounded-full transition-colors bg-border group-hover:bg-accent" />
          </div>
        </Tooltip>
        {!rightCollapsed && (
          <div ref={rightPanelRef} className="flex flex-col p-3 gap-3 overflow-hidden min-h-0" style={{ width: rightWidth, minWidth: 324, maxWidth: 400 }}>
            <div className="shrink-0"><ConnectionPanel onSelect={(w: WindowInfo) => { if (opStateRef.current === 'streaming') stopStream(); setSelWindow(w) }} onDisconnect={() => {
              if (opStateRef.current === 'streaming') stopStream()
              setSelWindow({ title: ' Entire Desktop', category: 'desktop', hwnd: 0 })
              setExpectedCaptureState('desktop')
              addLog('[Connection] disconnected, back to desktop')
            }} snapMethod={snapMethod} setSnapMethod={setSnapMethod} streamMethod={streamMethod} setStreamMethod={setStreamMethod} selWin={selWindow} winState={winState} expectedCaptureState={expectedCaptureState} setExpectedCaptureState={setExpectedCaptureState} expanded={connectionExpanded} onToggle={() => setConnExpanded(!connectionExpandedRef.current)} pinned={connectionPinned} onTogglePin={toggleConnPin} /></div>
            <div className="shrink-0 overflow-hidden"><ScreenshotPanel selWin={selWindow} screenRatio={screenRatio} snapMethod={snapMethod} streamMethod={streamMethod} renderMethod={renderMethod} winState={winState} expanded={screenshotExpanded} onToggle={() => setSsExpanded(!screenshotExpandedRef.current)} previewing={previewing} previewingRef={previewingRef} snapshotRef={snapshotRef} snapshotStartRef={snapshotStartRef} capMethod={capMethod} onTakeSnapshot={takeSnapshot} onTogglePreview={togglePreview} pinned={screenshotPinned} onTogglePin={toggleSsPin} hasContentRef={ssHasContentRef} /></div>
            <div className="shrink-0"><LogPanel compact expanded={logExpanded} onToggle={() => setLogPanelExpanded(!logExpandedRef.current)} pinned={logPinned} onTogglePin={toggleLogPin} /></div>
            <div className="flex-1" />
          </div>
        )}
      </div>
    </div>
  )
}
