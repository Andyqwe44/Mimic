import { useState, useEffect, useRef, useCallback } from 'react'
import { createPortal } from 'react-dom'
import { Play, Square, Camera, Monitor, Settings, Moon, Sun, ChevronDown, ChevronLeft, FileText, X, MonitorUp, Search, MonitorSmartphone, RefreshCw, FolderOpen, Cpu, Pencil, Copy, Check, ArrowDown, Unlink } from 'lucide-react'
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
const SELECTABLE_BTN = 'flex items-center w-full px-3 py-2 rounded-lg border transition-colors'

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
interface WindowInfo { title: string; category: string; hwnd: number }

// ═══ Window Picker ───
// ═══ Capture Mode Picker ───
const CAPTURE_MODES = [
  { v: 'foreground', label: '前台 (Foreground)', desc: '窗口可见且在最前 → 推荐 WGC GPU 加速', method: 'wgc' },
  { v: 'background', label: '后台 (Background)', desc: '窗口被遮挡但未最小化 → 推荐 WGC (唯一支持后台)', method: 'wgc' },
  { v: 'minimized',  label: '最小化 (Minimized)',  desc: '窗口已最小化 → 只能用 DesktopGDI 截桌面', method: 'dxgi' },
]

const PICKER_W = 'w-[520px]'
const PICKER_MAXH = 'max-h-[min(560px,85vh)]'

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
  const [processes, setProcesses] = useState<WindowInfo[]>([])
  const [filter, setFilter] = useState('all')
  const [loading, setLoading] = useState(false)
  // ── page 2: pending window (awaiting mode) ──
  const [pendingWin, _setPendingWin] = useState<WindowInfo | null>(null)

  // Reset on open — delay transition enable so stale page state doesn't animate
  useEffect(() => {
    if (open) {
      setPage('window'); setFilter('all'); setSearch(''); loadWindows(); setProcesses([])
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

  const loadProcesses = async () => {
    setLoading(true)
    try {
      const list = await hostCall('list_processes')
      setProcesses(list)
      addLog(`[Window] processes ${list.length} entries`)
    } catch {
      setProcesses([{ title: 'svchost.exe', category: 'process', hwnd: 0 }, { title: 'explorer.exe', category: 'process', hwnd: 0 }])
    }
    setLoading(false)
  }

  useEffect(() => {
    if (filter === 'process' && processes.length === 0 && open) { loadProcesses() }
  }, [filter])

  const categories = ['all', 'desktop', 'window', 'process'] as const
  const winHwnds = new Set(windows.map(w => w.hwnd))
  const allWindows = [...windows, ...processes.filter(p => !winHwnds.has(p.hwnd))]
  const filtered = allWindows.filter(w => {
    if (filter === 'desktop') return w.category === 'desktop'
    if (filter === 'window') return w.category === 'window'
    if (filter === 'process') return w.category === 'process'
    return true
  }).filter(w => w.title.toLowerCase().includes(search.toLowerCase()))

  const handlePickWindow = (w: WindowInfo) => {
    onSelectWindow(w)
    addLog(`[Window] ${w.title}`)
    if (w.hwnd === 0 || w.category === 'desktop') {
      onSelectMode('dxgi', 'desktop')
      addLog('[Capture] desktop → dxgi')
      onClose()
    } else {
      // DEV: skip mode picker page 2 — directly use wgc/foreground
      onSelectMode('wgc', 'foreground')
      addLog('[Capture] auto → wgc (mode picker skipped)')
      onClose()
      // setPendingWin(w)   // ← page 2 code preserved, not deleted
      // setPage('mode')
    }
  }

  const handlePickMode = (method: string, expectedState: string) => {
    onSelectMode(method, expectedState)
    addLog(`[Capture] mode=${expectedState} → ${method}`)
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
                <Tooltip key={c} text={`筛选: ${c === 'all' ? '全部' : c === 'desktop' ? '桌面' : c === 'window' ? '窗口' : '进程'}`}>
                  <button onClick={() => { setFilter(c); addLog(`[Filter] ${c}`) }}
                    className={`px-3 py-1 rounded-full text-xs font-medium transition-colors capitalize
                      ${filter === c ? 'bg-accent text-white' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'}`}>
                    {c === 'all' ? 'All' : c === 'desktop' ? ' Desktop' : c === 'window' ? ' Windows' : ' Process'}
                  </button>
                </Tooltip>
              ))}
              <div className="flex-1" />
              <Tooltip text="刷新窗口列表">
                <button onClick={() => { loadWindows(); setProcesses([]); addLog('[Window] refreshing list') }}
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
                  return (
                  <Tooltip key={`${w.hwnd}-${w.category}`} text={self ? '自身窗口，禁止捕获' : `选择: ${w.title}`}>
                    <button
                      disabled={self}
                      onClick={() => handlePickWindow(w)}
                    className={`w-full flex items-center gap-2 px-2 py-2 rounded-lg text-left transition-colors group min-w-0
                      ${self ? 'opacity-40 cursor-not-allowed' : 'hover:bg-bg-hover cursor-pointer'}`}>
                    {w.category === 'desktop' ? <MonitorSmartphone className="w-3.5 h-3.5 text-text-muted group-hover:text-accent shrink-0" />
                      : <Monitor className="w-3.5 h-3.5 text-text-muted group-hover:text-accent shrink-0" />}
                    <div className="flex-1 min-w-0">
                      <span className="text-xs text-text-primary truncate block">{w.title}</span>
                      <span className="text-xs text-text-muted capitalize">{w.category}</span>
                    </div>
                  </button>
                  </Tooltip>
                )})}
                </div>
              )}
            </div>
            <div className="px-4 py-2 border-t border-border text-xs text-text-muted text-center">
              {filtered.length} items — {windows.filter(w => w.category === 'desktop').length} desktops, {windows.filter(w => w.category === 'window').length} windows{processes.length > 0 ? `, ${processes.length} processes` : ''}
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
const STATE_LABEL: Record<string,string> = { desktop:'Desktop', foreground:'前台', background:'后台', minimized:'最小化', hidden:'隐藏', closed:'已关闭', unknown:'未知' }
const STATE_COLOR: Record<string,string> = { desktop:'text-text-muted', foreground:'text-success', background:'text-accent', minimized:'text-error', hidden:'text-error', closed:'text-error', unknown:'text-text-muted' }

// ═══ Connection Panel (MXU-style collapsible card) ═══
function ConnectionPanel({ onSelect, onDisconnect, forceMethod, setForceMethod, selWin, winState, expectedCaptureState, setExpectedCaptureState, autoMethod, setAutoMethod, showMethod, expanded, onToggle }: { onSelect: (w: WindowInfo) => void; onDisconnect?: () => void; forceMethod: string; setForceMethod?: (m: string) => void; selWin?: WindowInfo; winState: string; expectedCaptureState?: string; setExpectedCaptureState?: (s: string) => void; autoMethod?: boolean; setAutoMethod?: (v: boolean) => void; showMethod?: boolean; expanded: boolean; onToggle: () => void }) {
  const [pickerOpen, setPickerOpen] = useState(false)
  const [selTitle, setSelTitle] = useState(' Entire Desktop')
  const [ip, setIp] = useState('127.0.0.1')
  const [port, setPort] = useState('9999')
  const isDesktop = selTitle === ' Entire Desktop'

  const handleSelectWindow = (w: WindowInfo) => {
    setSelTitle(w.title); onSelect(w)
  }

  const handleSelectMode = (method: string, expectedState: string) => {
    if (setForceMethod) { setForceMethod(method) }
    if (setExpectedCaptureState) { setExpectedCaptureState(expectedState) }
  }

  // Sync selTitle when parent changes selWin externally
  useEffect(() => {
    if (selWin && selWin.title !== selTitle) {
      setSelTitle(selWin.title)
    }
  }, [selWin?.title])

  const cantCapture = !isDesktop && cantCaptureMinimized(forceMethod, winState)
  const recommendedMethod = winState === 'minimized' ? 'dxgi' : 'wgc'

  const methods = [
    { v: 'wgc',  name: 'WGC', eng: 'GPU FramePool', rec: '前台/后台', desc: 'GPU 加速，支持后台/遮挡窗口，前台后台首选' },
    { v: 'dxgi', name: 'DXGI', eng: 'DesktopBlt',   rec: '最小化/桌面', desc: '全桌面 GDI 位图，最小化窗口或桌面时唯一可行方案' },
  ]

  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
      <div role="button" tabIndex={0} onClick={() => { onToggle(); addLog(`[Connection] ${!expanded ? 'expanded' : 'collapsed'}`) }} onKeyDown={e=>{if(e.key==='Enter'||e.key===' '){(e.currentTarget as HTMLElement).click()}}}
        className={COLLAPSIBLE_HEADER}>
        <div className="flex items-center gap-2 min-w-0">
          <MonitorUp className="w-4 h-4 text-text-secondary shrink-0" />
          <span className="text-sm font-medium text-text-primary shrink-0">Connection</span>
        </div>
        <div className="flex items-baseline gap-2 ml-2">
          <span className="text-[10px] text-text-muted shrink-0">状态</span>
          <span className={`text-xs ${STATE_COLOR[winState] || 'text-text-muted'} shrink-0`}>
            {STATE_LABEL[winState] || winState}
          </span>
          <span className="text-[10px] text-text-muted shrink-0">推荐</span>
          <span className="text-xs text-accent truncate">{METHOD_SHORT[recommendedMethod] || recommendedMethod}</span>
          {cantCapture && <span className="text-xs text-error shrink-0">⚠</span>}
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
                窗口已最小化，{forceMethod.toUpperCase()} 无法截取。请切换为 WGC 或将窗口恢复前台。
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
            {showMethod && setForceMethod && setAutoMethod && (
              <div className="border-t border-border pt-2 space-y-2">
                <div className="flex items-center justify-between">
                  <span className="text-xs text-text-muted">Capture Method</span>
                  <label className="flex items-center gap-1.5 cursor-pointer select-none">
                    <span className="text-[10px] text-text-muted">Auto</span>
                    <button onClick={e => { e.stopPropagation(); setAutoMethod(!autoMethod); addLog(`[Setting] auto method = ${!autoMethod}`) }}
                      className={`relative w-8 h-5 rounded-full transition-colors ${autoMethod ? 'bg-amber-500' : 'bg-bg-tertiary'}`}>
                      <span className={`absolute top-[3px] w-3.5 h-3.5 rounded-full bg-white transition-transform ${autoMethod ? 'left-4' : 'left-0.5'}`} />
                    </button>
                  </label>
                </div>
                <div className="flex flex-col gap-3">{methods.map(m => {
                  const isActive = forceMethod === m.v
                  const ringClass = !autoMethod && isActive ? 'border-accent bg-accent/10 cursor-pointer'
                    : autoMethod && isActive ? 'border-amber-500 bg-amber-500/10 cursor-not-allowed'
                    : autoMethod ? 'border-border bg-bg-primary opacity-50 cursor-not-allowed'
                    : 'border-border bg-bg-primary hover:bg-bg-hover cursor-pointer'
                  return <Tooltip key={m.v} text={m.desc}><label className={`${SELECTABLE_BTN} ${ringClass}`}><input type="radio" name="method" value={m.v} checked={isActive} disabled={autoMethod} onChange={e => { if (!autoMethod) { setForceMethod(e.target.value); addLog(`[Setting] capture method = ${e.target.value}`) } }} className="sr-only" /><span className="text-xs font-medium text-text-primary">{m.name} <span className="text-text-muted">({m.eng})</span></span><span className="ml-auto text-xs font-medium text-text-primary">{m.rec}</span></label></Tooltip>
                })}</div>
              </div>
            )}
          </div>
        </div>
      </div>
      <TargetPickerModal open={pickerOpen} onClose={() => setPickerOpen(false)} onSelectWindow={handleSelectWindow} onSelectMode={handleSelectMode} />
    </div>
  )
}

// ═══ Screenshot Panel ───
function ScreenshotPanel({ selWin, screenRatio, forceMethod, transportMethod, winState, expanded, onToggle }: { selWin?: WindowInfo; screenRatio: number; forceMethod: string; transportMethod: string; winState: string; expanded: boolean; onToggle: () => void }) {
  const MJPEG_URL = 'http://127.0.0.1:9998/stream'
  const [previewing, setPreviewing] = useState(false)
  const [imgSrc, setImgSrc] = useState('')       // single-frame PNG / MJPEG fallback
  const [imgStyle, setImgStyle] = useState<React.CSSProperties>({})
  const [fps, setFps] = useState(0)
  const [capMethod, setCapMethod] = useState('')
  const previewingRef = useRef(false)
  const framesRef = useRef(0)
  const lastFpsRef = useRef(Date.now())
  const unlistenRef = useRef<(() => void) | null>(null)
  const sharedBufHandlerRef = useRef<((e: any) => void) | null>(null) // track sharedbufferreceived handler
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const [canvasDims, setCanvasDims] = useState({ w: 0, h: 0 })
  const sharedBufActiveRef = useRef(false)

  // ── SharedBuffer → Canvas rendering (zero-copy) ──
  const setupSharedBufferListener = () => {
    const wv = (window as any).chrome?.webview
    if (!wv) {
      sharedBufActiveRef.current = false
      setImgSrc(`${MJPEG_URL}?t=${Date.now()}`)
      addLog('[Preview] SharedBuffer not available, falling back to MJPEG')
      return
    }
    // Remove old listener if re-entering preview
    if (sharedBufHandlerRef.current) {
      wv.removeEventListener('sharedbufferreceived', sharedBufHandlerRef.current)
      sharedBufHandlerRef.current = null
    }
    const handler = (e: any) => {
      if (!previewingRef.current || !sharedBufActiveRef.current) return
      try {
        const buf: ArrayBuffer = e.getBuffer()
        const metaStr: string = e.getAdditionalData()
        const meta = JSON.parse(metaStr) as { w: number; h: number; ts: number }
        if (!meta.w || !meta.h || meta.w <= 0 || meta.h <= 0) return
        // Zero-copy: ArrayBuffer → Uint8ClampedArray → ImageData → Canvas
        // Note: C++ now sends RGBA (converted from BGRA), so ImageData works directly
        const imgData = new ImageData(
          new Uint8ClampedArray(buf, 0, meta.w * meta.h * 4),
          meta.w, meta.h
        )
        if (canvasRef.current) {
          canvasRef.current.width = meta.w
          canvasRef.current.height = meta.h
          setCanvasDims({ w: meta.w, h: meta.h })
          const ctx = canvasRef.current.getContext('2d')
          if (ctx) ctx.putImageData(imgData, 0, 0)
        }
        framesRef.current++
        const now = Date.now(); const elapsed = now - lastFpsRef.current
        if (elapsed >= 1000) { setFps(Math.round(framesRef.current * 1000 / elapsed)); framesRef.current = 0; lastFpsRef.current = now }
      } catch (_) { /* skip corrupt frame */ }
    }
    sharedBufHandlerRef.current = handler
    wv.addEventListener('sharedbufferreceived', handler)
    addLog('[Preview] SharedBuffer pipeline active — zero-copy Canvas')
  }

  // Compute proportional position within screen-aspect container
  const applyCaptureJson = (jsonStr: string) => {
    try {
      const info = JSON.parse(jsonStr)
      const src = `data:image/png;base64,${info.image}`
      setImgSrc(src)
      setCapMethod(info.method || '')
      // Proportional positioning: window rect mapped to screen-sized container
      if (info.screen_w && info.screen_h && info.w && info.h) {
        setImgStyle({
          position: 'absolute',
          left: `${(info.x / info.screen_w) * 100}%`,
          top: `${(info.y / info.screen_h) * 100}%`,
          width: `${(info.w / info.screen_w) * 100}%`,
          height: `${(info.h / info.screen_h) * 100}%`,
          objectFit: 'fill',
        })
      }
      return true
    } catch { return false }
  }

  // ── BMP Preview: Rust-native multi-method → BMP → <img> ──
  const togglePreview = async () => {
    if (previewing) {
      previewingRef.current = false; setPreviewing(false); setFps(0); setCapMethod('')
      sharedBufActiveRef.current = false
      if (unlistenRef.current) { unlistenRef.current(); unlistenRef.current = null }
      try { await hostCall('capture_stream_stop') } catch (_) {}
      setImgSrc('')
      addLog('[Preview] stopped')
    } else {
      const hwnd = selWin?.hwnd ?? 0
      if (cantCaptureMinimized(forceMethod, winState)) {
        addLog(`[Preview] blocked: window minimized, ${forceMethod} cannot capture`); return
      }
      addLog(`[Preview] ${selWin?.title ?? 'desktop'} [${forceMethod}]`)
      try { await hostCall('capture_stream_start', { hwnd, tcpPort: 9999, method: forceMethod, transport: transportMethod }) }
      catch (e) { addLog(`[Preview] start failed: ${e}`); return }

      // Auto-expand panel
      if (!expanded) onToggle()
      previewingRef.current = true; setPreviewing(true);
      setFps(0)

      // SharedBuffer (zero-copy): Canvas rendering, no MJPEG HTTP needed
      if (transportMethod === 'shared') {
        sharedBufActiveRef.current = true
        setImgSrc('') // hide <img>, show <canvas>
        setupSharedBufferListener()
      } else {
        // MJPEG stream — browser <img> natively handles multipart/x-mixed-replace
        setImgSrc(`${MJPEG_URL}?t=${Date.now()}`)
        sharedBufActiveRef.current = false
      }

      // Listen for 'tick' messages from C++ host for FPS counting
      const tickHandler = (e: any) => {
        if (!previewingRef.current) return
        try {
          const msg = JSON.parse(e.data)
          if (msg.type === 'tick') {
            if (msg.method) setCapMethod(msg.method)
            framesRef.current++
            const now = Date.now(); const elapsed = now - lastFpsRef.current
            if (elapsed >= 1000) { setFps(Math.round(framesRef.current * 1000 / elapsed)); framesRef.current = 0; lastFpsRef.current = now }
          }
        } catch {}
      }
      const wv = (window as any).chrome?.webview
      if (wv) {
        wv.addEventListener('message', tickHandler)
        unlistenRef.current = () => wv.removeEventListener('message', tickHandler)
      }
    }
  }

  // Cleanup: stop stream and unlisten on unmount
  useEffect(() => { return () => {
    previewingRef.current = false
    sharedBufActiveRef.current = false
    if (unlistenRef.current) { unlistenRef.current(); unlistenRef.current = null }
    const wv = (window as any).chrome?.webview
    if (wv && sharedBufHandlerRef.current) {
      wv.removeEventListener('sharedbufferreceived', sharedBufHandlerRef.current)
      sharedBufHandlerRef.current = null
    }
    // Stop backend stream to prevent resource leak
    hostCall('capture_stream_stop').catch(() => {})
  } }, [])

  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
      <div role="button" tabIndex={0} onClick={() => { onToggle(); addLog(`[Screenshot] ${!expanded ? 'expanded' : 'collapsed'}`) }} onKeyDown={e=>{if(e.key==='Enter'||e.key===' '){(e.currentTarget as HTMLElement).click()}}}
        className={COLLAPSIBLE_HEADER}>
        <div className="flex items-center gap-2 min-w-0">
          <Camera className="w-4 h-4 text-text-secondary shrink-0" />
          <span className="text-sm font-medium text-text-primary shrink-0">Screenshot</span>
          {previewing && <span className="text-xs text-accent shrink-0">{fps} FPS</span>}
          {capMethod && !previewing && <span className="text-xs text-text-muted shrink-0">{capMethod}</span>}
        </div>
        <div className="flex items-center gap-0.5 shrink-0">
          <Tooltip text="单帧截图">
            <button onClick={e => { e.stopPropagation(); (async () => {
              const hwnd = selWin?.hwnd ?? 0;
              if (cantCaptureMinimized(forceMethod, winState)) {
                addLog(`[Capture] blocked: window minimized, ${forceMethod} cannot capture`); return
              }
              addLog(`[Capture] ${METHOD_SHORT[forceMethod] || forceMethod} ${hwnd ? 'hwnd='+hwnd : 'desktop'}...`)
              const t0 = Date.now()
              try {
                const json = await hostCall('capture_window', { hwnd, method: forceMethod })
                const elapsed = Date.now() - t0
                if (applyCaptureJson(json)) {
                  try { const info = JSON.parse(json); addLog(`[Capture] OK (${elapsed}ms) [${info.method}]`) }
                  catch { addLog(`[Capture] OK (${elapsed}ms)`) }
                }
                else { setImgSrc(''); setImgStyle({}); addLog(`[Capture] failed after ${elapsed}ms`) }
              } catch { addLog(`[Capture] failed after ${Date.now() - t0}ms`) }
            })() }}
              className="p-1 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors">
              <Camera className="w-3.5 h-3.5" />
            </button>
          </Tooltip>
          {previewing
            ? <Tooltip text="停止实时预览"><button onClick={e => { e.stopPropagation(); togglePreview() }}
                className="p-1 rounded-md text-success hover:bg-bg-tertiary transition-colors"><Square className="w-3.5 h-3.5" /></button></Tooltip>
            : <Tooltip text="开始实时预览"><button onClick={e => { e.stopPropagation(); togglePreview() }}
                className="p-1 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors"><Play className="w-3.5 h-3.5" /></button></Tooltip>
          }
          <ChevronDown className={`w-4 h-4 text-text-muted transition-transform duration-150 ${expanded?'rotate-180':''}`} />
        </div>
      </div>
      <div className="grid transition-[grid-template-rows] duration-150 ease-out"
        style={{ gridTemplateRows: expanded ? '1fr' : '0fr' }}>
        <div className="overflow-hidden min-h-0" data-layout-measure="">
          <div className="border-t border-border" />
          <div className="p-3">
            <div className="w-full rounded-lg bg-bg-primary overflow-hidden flex items-center justify-center relative"
              style={{ aspectRatio: screenRatio }}>
              {previewing ? (
                sharedBufActiveRef.current ? (
                  // SharedBuffer mode: raw BGRA → Canvas (zero-copy)
                  <canvas ref={canvasRef}
                    className="max-w-full max-h-full object-contain"
                    style={{ aspectRatio: canvasDims.w && canvasDims.h ? `${canvasDims.w}/${canvasDims.h}` : '16/9' }} />
                ) : (
                  // MJPEG fallback: <img> GPU hardware decode
                  <img src={imgSrc || `${MJPEG_URL}?t=${Date.now()}`}
                    className="max-w-full max-h-full object-contain"
                    alt="MJPEG stream" />
                )
              ) : imgSrc ? (
                <img src={imgSrc} style={imgStyle} alt="preview" />
              ) : (
                <span className="text-sm text-text-muted">Press Preview</span>
              )}
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

function LogPanel({ compact, expanded: exp, onToggle, keepFiles }: { compact?: boolean; expanded?: boolean; onToggle?: () => void; keepFiles?: number }) {
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

  // Load history files from disk on mount
  useEffect(() => {
    logMgr.loadHistory(keepFiles ?? 5).then(setHistoryFiles)
  }, [keepFiles])

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

function SettingsView({ forceMethod, setForceMethod, autoMethod, setAutoMethod, transportMethod, setTransportMethod, autoTransport, setAutoTransport, selWin, winState, expectedCaptureState, setExpectedCaptureState, onSelect, onDisconnect, keepFiles, setKeepFiles, appVersion, theme, setTheme }: { forceMethod: string; setForceMethod: (m: string) => void; autoMethod?: boolean; setAutoMethod?: (v: boolean) => void; transportMethod: string; setTransportMethod: (m: string) => void; autoTransport?: boolean; setAutoTransport?: (v: boolean) => void; selWin?: WindowInfo; winState: string; expectedCaptureState?: string; setExpectedCaptureState?: (s: string) => void; onSelect: (w: WindowInfo) => void; onDisconnect: () => void; keepFiles: number; setKeepFiles: (n: number) => void; appVersion: string; theme: string; setTheme: (t: 'light'|'dark'|'system') => void }) {
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

      <ConnectionPanel onSelect={onSelect} onDisconnect={onDisconnect} forceMethod={forceMethod} setForceMethod={setForceMethod} selWin={selWin} winState={winState} expectedCaptureState={expectedCaptureState} setExpectedCaptureState={setExpectedCaptureState} autoMethod={autoMethod} setAutoMethod={setAutoMethod} showMethod expanded={connExpanded} onToggle={() => setConnExpanded(v => !v)} />

      <SettingsCard icon={<Camera className="w-4 h-4 text-text-secondary" />} title="Capture">
        <div className="space-y-2">
          <div className="flex items-center justify-between">
            <span className="text-xs text-text-muted">How frames are sent to the frontend for preview.</span>
            {setAutoTransport && (
              <label className="flex items-center gap-1.5 cursor-pointer select-none shrink-0">
                <span className="text-[10px] text-text-muted">Auto</span>
                <button onClick={e => { e.stopPropagation(); setAutoTransport(!autoTransport); addLog(`[Setting] auto transport = ${!autoTransport}`) }}
                  className={`relative w-8 h-5 rounded-full transition-colors ${autoTransport ? 'bg-amber-500' : 'bg-bg-tertiary'}`}>
                  <span className={`absolute top-[3px] w-3.5 h-3.5 rounded-full bg-white transition-transform ${autoTransport ? 'left-4' : 'left-0.5'}`} />
                </button>
              </label>
            )}
          </div>
          <div className="flex flex-col gap-3">
            {[
              { v:'shared', name:'Canvas', eng:'SharedBuffer', rec:'首选', desc:'Zero-copy BGRA → Canvas — no encode, no HTTP, lowest latency' },
              { v:'mjpeg',  name:'MJPEG',  eng:'HTTP Stream',  rec:'备用', desc:'JPEG stream via HTTP, browser GPU decode — stable fallback' },
              { v:'base64', name:'Base64', eng:'JSON',         rec:'旧版', desc:'Raw RGBA via JSON/base64 — legacy, slow' },
              { v:'h264',   name:'H.264',  eng:'GPU MFT',      rec:'实验', desc:'GPU MFT encode — experimental' },
            ].map(t => {
              const isActive = transportMethod === t.v
              const ringClass = !autoTransport && isActive ? 'border-accent bg-accent/10 cursor-pointer'
                : autoTransport && isActive ? 'border-amber-500 bg-amber-500/10 cursor-not-allowed'
                : autoTransport ? 'border-border bg-bg-primary opacity-50 cursor-not-allowed'
                : 'border-border bg-bg-primary hover:bg-bg-hover cursor-pointer'
              return <Tooltip key={t.v} text={t.desc}><label className={`${SELECTABLE_BTN} ${ringClass}`}><input type="radio" name="transport" value={t.v} checked={isActive} disabled={autoTransport} onChange={e => { if (!autoTransport) { setTransportMethod(e.target.value); addLog(`[Transport] ${e.target.value}`) } }} className="sr-only" /><span className="text-xs font-medium text-text-primary">{t.name} <span className="text-text-muted">({t.eng})</span></span><span className="ml-auto text-xs font-medium text-text-primary">{t.rec}</span></label></Tooltip>
            })}
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
        </div>
      </SettingsCard>

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
  const [screenshotExpanded, setScreenshotExpanded] = useState(true)
  const [logExpanded, setLogExpanded] = useState(true)
  const connectionExpandedRef = useRef(connectionExpanded)
  connectionExpandedRef.current = connectionExpanded
  const screenshotExpandedRef = useRef(screenshotExpanded)
  screenshotExpandedRef.current = screenshotExpanded
  const logExpandedRef = useRef(logExpanded)
  logExpandedRef.current = logExpanded
  const rightPanelRef = useRef<HTMLDivElement>(null)

  // ── Theme state (shared by ThemeBtn + Settings General) ──
  const [theme, setTheme] = useState<'light'|'dark'|'system'>('dark')
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
  // expanded overflow the right panel, collapse L → S → C until they fit.
  useEffect(() => {
    const check = () => {
      measureLayout()
      const el = rightPanelRef.current; if (!el) return
      const kids = el.querySelectorAll(':scope > div')
      let kidsH = 0
      for (let i = 0; i < 3; i++) kidsH += (kids[i] as HTMLElement).offsetHeight
      const overflow = kidsH + GAP - el.clientHeight
      if (overflow > 4) {
        if (logExpandedRef.current) {
          addLog(`[Layout] init overflow ${overflow}px → auto-collapse log`)
          setLogExpanded(false)
          setTimeout(check, 250) // re-check after CSS animation
        } else if (screenshotExpandedRef.current) {
          addLog(`[Layout] init overflow ${overflow}px → auto-collapse screenshot`)
          setScreenshotExpanded(false)
          setTimeout(check, 250)
        } else if (connectionExpandedRef.current) {
          addLog(`[Layout] init overflow ${overflow}px → auto-collapse connection`)
          setConnectionExpanded(false)
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
        // Shrinking: actual overflow → collapse L → S → C (one per event)
        if (overflow > 4) {
          if (logExpandedRef.current && now - guard.current.L > 350) {
            addLog(`[Layout] overflow ${overflow}px → auto-collapse log`)
            setLogExpanded(false); guard.current.L = now
          } else if (screenshotExpandedRef.current && now - guard.current.S > 350) {
            addLog(`[Layout] overflow ${overflow}px → auto-collapse screenshot`)
            setScreenshotExpanded(false); guard.current.S = now
          } else if (connectionExpandedRef.current && now - guard.current.C > 350) {
            addLog(`[Layout] overflow ${overflow}px → auto-collapse connection`)
            setConnectionExpanded(false); guard.current.C = now
          }
        }
      } else if (ch > prev) {
        // Growing: if room for next collapsed panel, expand C → S → L (one per event)
        if (!connectionExpandedRef.current && now - guard.current.C > 350) {
          const wouldNeed = (kidsH - h.Cp + h.C) + GAP
          if (ch >= wouldNeed) {
            addLog(`[Layout] room for C (need ${wouldNeed}px) → auto-expand connection`)
            setConnectionExpanded(true); guard.current.C = now
          }
        } else if (!screenshotExpandedRef.current && now - guard.current.S > 350) {
          const wouldNeed = (kidsH - h.Sp + h.S) + GAP
          if (ch >= wouldNeed) {
            addLog(`[Layout] room for S (need ${wouldNeed}px) → auto-expand screenshot`)
            setScreenshotExpanded(true); guard.current.S = now
          }
        } else if (!logExpandedRef.current && now - guard.current.L > 350) {
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
      // Collapse L → S → C
      if (logExpandedRef.current) {
        addLog(`[Layout] drag overflow ${overflow}px → auto-collapse log`)
        setLogExpanded(false)
      } else if (screenshotExpandedRef.current) {
        addLog(`[Layout] drag overflow ${overflow}px → auto-collapse screenshot`)
        setScreenshotExpanded(false)
      } else if (connectionExpandedRef.current) {
        addLog(`[Layout] drag overflow ${overflow}px → auto-collapse connection`)
        setConnectionExpanded(false)
      }
    } else if (overflow < -4) {
      // Room available — expand C → S → L
      if (!connectionExpandedRef.current) {
        const wouldNeed = (kidsH - h.Cp + h.C) + GAP
        if (ch >= wouldNeed) {
          addLog(`[Layout] drag room for C (need ${wouldNeed}px) → auto-expand connection`)
          setConnectionExpanded(true)
        }
      } else if (!screenshotExpandedRef.current) {
        const wouldNeed = (kidsH - h.Sp + h.S) + GAP
        if (ch >= wouldNeed) {
          addLog(`[Layout] drag room for S (need ${wouldNeed}px) → auto-expand screenshot`)
          setScreenshotExpanded(true)
        }
      } else if (!logExpandedRef.current) {
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
  const [forceMethod, setForceMethod] = useState('dxgi')
  const [autoMethod, setAutoMethod] = useState(true)
  const [expectedCaptureState, setExpectedCaptureState] = useState('desktop')
  const [transportMethod, setTransportMethod] = useState('shared')
  const [autoTransport, setAutoTransport] = useState(true)
  const [keepFiles, setKeepFiles] = useState(5)
  const [winState, setWinState] = useState('desktop')
  const lastWinStateRef = useRef('desktop')

  useEffect(() => {
    (async () => {
      try {
        const si = await hostCall('screen_info')
        setScreenRatio(si.w / si.h)
      } catch (_) {}
    })()
  }, [])

  // Yellow border overlay on selected window
  useEffect(() => {
    (async () => {
      try {
        await hostCall('highlight_window', { hwnd: selWindow.hwnd })
      } catch (_) {}
    })()
    return () => {
      (async () => {
        try {
          await hostCall('highlight_window', { hwnd: 0 })
        } catch (_) {}
      })()
    }
  }, [selWindow.hwnd])

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

  // Auto-select capture method based on actual window state
  useEffect(() => {
    if (autoMethod) {
      setForceMethod(winState === 'minimized' ? 'dxgi' : 'wgc')
    }
  }, [winState, autoMethod])

  // Auto-select transport based on availability (SharedBuffer > MJPEG > Base64)
  useEffect(() => {
    if (autoTransport) {
      setTransportMethod('shared')
    }
  }, [autoTransport])

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
          {tab === 'Settings' && <SettingsView forceMethod={forceMethod} setForceMethod={setForceMethod} autoMethod={autoMethod} setAutoMethod={setAutoMethod} transportMethod={transportMethod} setTransportMethod={setTransportMethod} autoTransport={autoTransport} setAutoTransport={setAutoTransport} selWin={selWindow} winState={winState} expectedCaptureState={expectedCaptureState} setExpectedCaptureState={setExpectedCaptureState} onSelect={setSelWindow} onDisconnect={() => { setSelWindow({ title: ' Entire Desktop', category: 'desktop', hwnd: 0 }); setExpectedCaptureState('desktop'); addLog('[Connection] disconnected, back to desktop') }} keepFiles={keepFiles} setKeepFiles={setKeepFiles} appVersion={appVersion} theme={theme} setTheme={setTheme} />}
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
            <div className="shrink-0"><ConnectionPanel onSelect={setSelWindow} onDisconnect={() => {
              setSelWindow({ title: ' Entire Desktop', category: 'desktop', hwnd: 0 })
              setExpectedCaptureState('desktop')
              addLog('[Connection] disconnected, back to desktop')
            }} forceMethod={forceMethod} setForceMethod={setForceMethod} selWin={selWindow} winState={winState} expectedCaptureState={expectedCaptureState} setExpectedCaptureState={setExpectedCaptureState} autoMethod={autoMethod} setAutoMethod={setAutoMethod} expanded={connectionExpanded} onToggle={() => setConnectionExpanded(v => !v)} /></div>
            <div className="shrink-0 overflow-hidden"><ScreenshotPanel selWin={selWindow} screenRatio={screenRatio} forceMethod={forceMethod} transportMethod={transportMethod} winState={winState} expanded={screenshotExpanded} onToggle={() => setScreenshotExpanded(v => !v)} /></div>
            <div className="shrink-0"><LogPanel compact expanded={logExpanded} onToggle={() => setLogExpanded(v => !v)} /></div>
            <div className="flex-1" />
          </div>
        )}
      </div>
    </div>
  )
}
