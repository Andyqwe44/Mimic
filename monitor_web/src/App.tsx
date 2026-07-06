import { useState, useEffect, useRef, useCallback } from 'react'
import { createPortal } from 'react-dom'
import { Play, Square, Camera, Monitor, Settings, Moon, Sun, ChevronDown, FileText, Trash2, X, MonitorUp, Search, MonitorSmartphone, RefreshCw } from 'lucide-react'
import { invoke } from '@tauri-apps/api/core'
import { listen } from '@tauri-apps/api/event'

// ═══ Tooltip ── 300ms delay, portal to body, smart positioning ═══
function Tooltip({ text, children }: { text: string; children: React.ReactElement }) {
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
    <div ref={ref} className="relative inline-flex"
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

// ═══ Theme btn ───
function ThemeBtn() {
  const [dark, setDark] = useState(false)
  return (
    <Tooltip text={dark ? "切换亮色主题" : "切换暗色主题"}>
      <button onClick={() => { const d = !dark; setDark(d); document.documentElement.classList.toggle('dark', d); addLog(`Theme: ${d ? 'dark' : 'light'}`) }}
        className="p-2 rounded-md hover:bg-bg-hover transition-colors">
        {dark ? <Sun className="w-4 h-4 text-text-secondary" /> : <Moon className="w-4 h-4 text-text-secondary" />}
      </button>
    </Tooltip>
  )
}

// ═══ Reusable components with REQUIRED title (compile-time enforced) ═══
function IconBtn({ icon, onClick, title, active }: { icon: React.ReactNode; onClick?: () => void; title: string; active?: boolean }) {
  return (
    <Tooltip text={title}>
      <button onClick={onClick}
        className={`p-2 rounded-md transition-colors ${active ? 'bg-accent/10 text-accent' : 'hover:bg-bg-hover text-text-secondary'}`}>
        {icon}
      </button>
    </Tooltip>
  )
}

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
function TopBar({ tab, setTab, running, onStart, onStop }: {
  tab: string; setTab: (t: 'Dashboard'|'Monitor'|'Log'|'Settings') => void; running: boolean; onStart: () => void; onStop: () => void
}) {
  const tabs = [
    { id: 'Dashboard' as const, icon: <Settings className="w-3.5 h-3.5" />, label: 'Dashboard' },
    { id: 'Monitor' as const, icon: <Monitor className="w-3.5 h-3.5" />, label: 'Monitor' },
    { id: 'Log' as const, icon: <FileText className="w-3.5 h-3.5" />, label: 'Log' },
  ]
  return (
    <div className="flex items-center h-10 bg-bg-secondary border-b border-border select-none shrink-0">
      <div className="flex-1 flex items-center h-full overflow-x-auto">
        {tabs.map(t => (
          <button key={t.id} onClick={() => { setTab(t.id); addLog(`Tab: ${t.label}`) }}
            className={`group flex items-center gap-1.5 h-full px-3 cursor-pointer border-r border-border min-w-[100px] transition-colors
              ${t.id === tab ? 'bg-bg-primary text-accent border-b-[3px] border-b-accent' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover border-b-[3px] border-b-transparent'}`}>
            {t.icon}
            <span className="text-sm font-medium">{t.label}</span>
          </button>
        ))}
      </div>
      <div className="flex items-center gap-1 px-2">
        {running
          ? <ActionBtn icon={<Square className="w-3.5 h-3.5" />} label="Stop" title="停止所有运行中的任务" variant="danger" onClick={() => { onStop(); addLog('Action: Stop') }} />
          : <ActionBtn icon={<Play className="w-3.5 h-3.5" />} label="Start" title="启动agent任务" variant="primary" onClick={() => { onStart(); addLog('Action: Start') }} />
        }
        <div className="mx-1 h-4 w-px bg-border" />
        <ThemeBtn />
        <IconBtn title="设置" icon={<Settings className="w-4 h-4" />} onClick={() => { setTab('Settings'); addLog('Tab: Settings') }} />
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
function WindowPickerModal({ open, onClose, onSelect }: { open: boolean; onClose: () => void; onSelect: (w: WindowInfo) => void }) {
  const [search, setSearch] = useState('')
  const [windows, setWindows] = useState<WindowInfo[]>([])
  const [processes, setProcesses] = useState<WindowInfo[]>([])
  const [filter, setFilter] = useState('all')
  const [loading, setLoading] = useState(false)

  useEffect(() => { if (open) { setFilter('all'); loadWindows(); setProcesses([]) } }, [open])

  const loadWindows = async () => {
    setLoading(true)
    try {
      const list = await invoke<WindowInfo[]>('list_windows')
      setWindows(list)
      addLog(`Windows loaded: ${list.length} entries`)
    } catch {
      setWindows([{ title: ' Entire Desktop', category: 'desktop', hwnd: 0 }, { title: 'Tic Tac Toe — main.exe', category: 'window', hwnd: 0 }, { title: 'Notepad', category: 'window', hwnd: 0 }, { title: 'Chrome', category: 'window', hwnd: 0 }])
    }
    setLoading(false)
  }

  const loadProcesses = async () => {
    setLoading(true)
    try {
      const list = await invoke<WindowInfo[]>('list_processes')
      setProcesses(list)
      addLog(`Processes loaded: ${list.length} entries`)
    } catch {
      setProcesses([{ title: 'svchost.exe', category: 'process', hwnd: 0 }, { title: 'explorer.exe', category: 'process', hwnd: 0 }])
    }
    setLoading(false)
  }

  // Lazy-load processes when Process filter clicked
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

  if (!open) return null
  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center">
      <div className="absolute inset-0 bg-black/30" onClick={onClose} />
      <div className="relative w-[520px] max-h-[560px] bg-bg-secondary border border-border rounded-xl shadow-lg flex flex-col">
        <div className="flex items-center justify-between px-4 py-3 border-b border-border">
          <div className="flex items-center gap-2">
            <MonitorUp className="w-4 h-4 text-accent" />
            <span className="text-sm font-semibold text-text-primary">Select Target</span>
          </div>
          <Tooltip text="关闭"><button onClick={onClose} className="p-1 rounded-md hover:bg-bg-hover transition-colors"><X className="w-4 h-4 text-text-secondary" /></button></Tooltip>
        </div>
        {/* Category tabs + Refresh */}
        <div className="flex items-center gap-1 px-4 pt-3 pb-1">
          {categories.map(c => (
            <Tooltip text={`筛选: ${c === 'all' ? '全部' : c === 'desktop' ? '桌面' : c === 'window' ? '窗口' : '进程'}`}>
              <button key={c} onClick={() => setFilter(c)}
                className={`px-3 py-1 rounded-full text-xs font-medium transition-colors capitalize
                  ${filter === c ? 'bg-accent text-white' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'}`}>
                {c === 'all' ? 'All' : c === 'desktop' ? ' Desktop' : c === 'window' ? ' Windows' : ' Process'}
              </button>
            </Tooltip>
          ))}
          <div className="flex-1" />
          <Tooltip text="刷新窗口列表">
            <button onClick={() => { loadWindows(); setProcesses([]); addLog('Refreshing windows...') }}
              className={`p-1.5 rounded-md hover:bg-bg-hover transition-colors text-text-secondary ${loading ? 'animate-spin' : ''}`}>
              <RefreshCw className="w-3.5 h-3.5" />
            </button>
          </Tooltip>
        </div>
        {/* Search */}
        <div className="px-4 py-2">
          <div className="flex items-center gap-2 h-8 rounded-lg border border-border bg-bg-primary px-3">
            <Search className="w-3.5 h-3.5 text-text-muted shrink-0" />
            <Tooltip text="输入关键字筛选窗口列表">
              <input value={search} onChange={e => setSearch(e.target.value)} placeholder="Search..."
                className="flex-1 bg-transparent text-sm text-text-primary outline-none placeholder:text-text-muted" autoFocus />
            </Tooltip>
          </div>
        </div>
        {/* List */}
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
                  onClick={() => { onSelect(w); onClose() }}
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
    </div>
  )
}

// ═══ Connection Panel ───
// ═══ Connection Panel (MXU-style collapsible card) ═══
function ConnectionPanel({ onSelect, onDisconnect }: { onSelect: (w: WindowInfo) => void; onDisconnect?: () => void }) {
  const [expanded, setExpanded] = useState(true)
  const [pickerOpen, setPickerOpen] = useState(false)
  const [title, setTitle] = useState(' Entire Desktop')
  const [ip, setIp] = useState('127.0.0.1')
  const [port, setPort] = useState('9999')
  const handleSelect = (w: WindowInfo) => { setTitle(w.title); onSelect(w); addLog(`Selected: ${w.title}`) }
  const isDesktop = title === ' Entire Desktop'

  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
      <div role="button" tabIndex={0} onClick={() => setExpanded(!expanded)} onKeyDown={e=>{if(e.key==='Enter'||e.key===' '){(e.currentTarget as HTMLElement).click()}}}
        className="w-full flex items-center justify-between px-3 py-2 hover:bg-bg-hover cursor-pointer transition-colors outline-none">
        <div className="flex items-center gap-2">
          <MonitorUp className="w-4 h-4 text-text-secondary" />
          <span className="text-sm font-medium text-text-primary">Connection</span>
          <span className={`text-xs ml-1 truncate max-w-[100px] ${isDesktop ? 'text-text-muted' : 'text-success'}`}>
            {isDesktop ? 'Desktop' : title}
          </span>
        </div>
        <ChevronDown className={`w-4 h-4 text-text-muted transition-transform duration-150 ${expanded?'rotate-180':''}`} />
      </div>
      <div className="grid transition-[grid-template-rows] duration-150 ease-out"
        style={{ gridTemplateRows: expanded ? '1fr' : '0fr' }}>
        <div className="overflow-hidden min-h-0">
          <div className="border-t border-border" />
          <div className="p-3 space-y-2">
            <div className="flex justify-between">
              <div className="flex items-center gap-1.5">
                <Tooltip text="已选择的目标窗口（只读，请用Select选择）">
                  <input value={title} readOnly placeholder="Window Title"
                    className="w-36 h-8 rounded-lg border border-border bg-bg-primary px-2 text-xs text-text-primary outline-none cursor-default text-text-muted truncate" />
                </Tooltip>
                {onDisconnect && (
                  <Tooltip text="断开当前窗口连接，回到桌面">
                    <button onClick={() => { onDisconnect(); setTitle(' Entire Desktop') }}
                      className="h-8 w-8 flex items-center justify-center rounded-md bg-red-600 hover:bg-red-700 text-white transition-colors shrink-0">
                      <X className="w-3.5 h-3.5" />
                    </button>
                  </Tooltip>
                )}
              </div>
              <ActionBtn icon={<MonitorUp className="w-3.5 h-3.5" />} label="Select" title="选择要捕获的窗口或桌面" variant="primary" onClick={() => setPickerOpen(true)} className="h-8" />
            </div>
            <div className="flex justify-between">
              <Tooltip text="AI模型服务器IP地址">
                <input value={ip} onChange={e => { const v=e.target.value; if(v.includes('::')){const[a,b]=v.split('::',2);setIp(a.trim());if(b?.trim())setPort(b.trim())}else setIp(v) }} placeholder="IP Address"
                  className="w-[184px] h-8 rounded-lg border border-border bg-bg-primary px-2 text-xs text-text-primary outline-none focus:border-accent transition-colors placeholder:text-text-muted" />
              </Tooltip>
              <Tooltip text="Port端口号">
                <input value={port} onChange={e => { const v=e.target.value; if(v.includes('::')){const[a,b]=v.split('::',2);if(a?.trim())setIp(a.trim());setPort(b?.trim()??'')}else setPort(v) }} placeholder="Port"
                  className="w-20 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm text-text-primary outline-none focus:border-accent transition-colors placeholder:text-text-muted" />
              </Tooltip>
            </div>
          </div>
        </div>
      </div>
      <WindowPickerModal open={pickerOpen} onClose={() => setPickerOpen(false)} onSelect={handleSelect} />
    </div>
  )
}

// ═══ Screenshot Panel ───
function ScreenshotPanel({ selWin, screenRatio, forceMethod, setForceMethod }: { selWin?: WindowInfo; screenRatio: number; forceMethod: string; setForceMethod: (m: string) => void }) {
  const [expanded, setExpanded] = useState(true)
  const [previewing, setPreviewing] = useState(false)
  const [imgSrc, setImgSrc] = useState('')       // single-frame PNG (Camera btn)
  const [imgStyle, setImgStyle] = useState<React.CSSProperties>({})
  const [fps, setFps] = useState(0)
  const [capMethod, setCapMethod] = useState('')
  const previewingRef = useRef(false)
  const framesRef = useRef(0)
  const lastFpsRef = useRef(Date.now())
  const unlistenRef = useRef<(() => void) | null>(null)
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const [canvasDims, setCanvasDims] = useState({ w: 0, h: 0 })

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
      if (unlistenRef.current) { unlistenRef.current(); unlistenRef.current = null }
      try { await invoke<string>('capture_stream_stop') } catch (_) {}
      setImgSrc('')
      addLog('Preview stopped')
    } else {
      const hwnd = selWin?.hwnd ?? 0
      const method = forceMethod === 'auto' ? null : forceMethod
      addLog(`Preview: ${selWin?.title ?? 'desktop'} [${method ?? 'auto'}]`)
      try { await invoke<string>('capture_stream_start', { hwnd, tcpPort: 9999, method }) }
      catch (e) { addLog(`Stream start failed: ${e}`); return }

      previewingRef.current = true; setPreviewing(true); setImgSrc('')
      framesRef.current = 0; lastFpsRef.current = Date.now()

      const unlisten = await listen<{ method: string }>('stream-tick', async (event) => {
        if (!previewingRef.current) return
        if (event.payload.method) setCapMethod(event.payload.method)
        try {
          const json = await invoke<string>('stream_poll')
          if (json && canvasRef.current) {
            const data = JSON.parse(json)  // {p: base64, w, h, m}
            const canvas = canvasRef.current
            if (canvas.width !== data.w || canvas.height !== data.h) {
              canvas.width = data.w; canvas.height = data.h
              setCanvasDims({ w: data.w, h: data.h })
            }
            const ctx = canvas.getContext('2d')
            if (ctx) {
              const binary = atob(data.p)
              const bytes = new Uint8Array(binary.length)
              for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i)
              const imgData = new ImageData(new Uint8ClampedArray(bytes.buffer), data.w, data.h)
              ctx.putImageData(imgData, 0, 0)
            }
          }
        } catch (_) {}
        framesRef.current++
        const now = Date.now(); const elapsed = now - lastFpsRef.current
        if (elapsed >= 1000) { setFps(Math.round(framesRef.current * 1000 / elapsed)); framesRef.current = 0; lastFpsRef.current = now }
      })
      unlistenRef.current = unlisten
    }
  }

  // Cleanup: stop stream and unlisten on unmount
  useEffect(() => { return () => {
    previewingRef.current = false
    if (unlistenRef.current) { unlistenRef.current(); unlistenRef.current = null }
    // Stop backend stream to prevent resource leak
    invoke<string>('capture_stream_stop').catch(() => {})
  } }, [])

  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
      <div role="button" tabIndex={0} onClick={() => setExpanded(!expanded)} onKeyDown={e=>{if(e.key==='Enter'||e.key===' '){(e.currentTarget as HTMLElement).click()}}}
        className="w-full flex items-center justify-between px-3 py-2 hover:bg-bg-hover cursor-pointer transition-colors outline-none">
        <div className="flex items-center gap-2">
          <Camera className="w-4 h-4 text-text-secondary" />
          <span className="text-sm font-medium text-text-primary">Screenshot</span>
          {previewing && <span className="text-xs text-accent">{fps} FPS</span>}
          {previewing && forceMethod !== 'auto' && <span className="text-xs text-success ml-1">{forceMethod}</span>}
          {capMethod && !previewing && <span className="text-xs text-success">{capMethod}</span>}
        </div>
        <div className="flex items-center gap-0.5">
          <Tooltip text="单帧截图">
            <button onClick={e => { e.stopPropagation(); (async () => {
              const hwnd = selWin?.hwnd ?? 0;
              const method = forceMethod === 'auto' ? null : forceMethod
              addLog(`Capture [${method ?? 'auto'}] ${hwnd ? 'hwnd='+hwnd : 'desktop'}...`)
              const t0 = Date.now()
              try {
                const json = await invoke<string>('capture_window', { hwnd, method })
                const elapsed = Date.now() - t0
                if (applyCaptureJson(json)) { addLog(`Screenshot [${method ?? 'auto'}] OK (${elapsed}ms)`) }
                else { setImgSrc(''); setImgStyle({}); addLog(`Screenshot [${method ?? 'auto'}] failed after ${elapsed}ms`) }
              } catch { addLog(`Screenshot [${method ?? 'auto'}] failed after ${Date.now() - t0}ms`) }
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
        <div className="overflow-hidden min-h-0">
          <div className="border-t border-border" />
          <div className="p-3">
            <div className="w-full rounded-lg bg-bg-primary overflow-hidden flex items-center justify-center relative"
              style={{ aspectRatio: screenRatio }}>
              {previewing ? (
                <canvas ref={canvasRef} className="max-w-full max-h-full object-contain"
                  style={{ width: canvasDims.w, height: canvasDims.h }} />
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


// ═══ Shared log state ───
type LogEntry = { ts: string; msg: string }
let gLogs: LogEntry[] = []
let gLogListeners: (() => void)[] = []
function addLog(msg: string) { gLogs = [...gLogs, { ts: new Date().toLocaleTimeString(), msg }]; gLogListeners.forEach(f => f()) }

// ═══ Log Panel ───
type HistoryFile = { name: string; lines: string[] }

function LogPanel({ compact }: { compact?: boolean }) {
  const [expanded, setExpanded] = useState(true)
  const [logs, setLogs] = useState(gLogs)
  const [historyFiles, setHistoryFiles] = useState<HistoryFile[]>([])
  const [historyLoaded, setHistoryLoaded] = useState(false)
  const [openFiles, setOpenFiles] = useState<Set<number>>(new Set())
  useEffect(() => { const fn = () => setLogs([...gLogs]); gLogListeners.push(fn); return () => { gLogListeners = gLogListeners.filter(f => f !== fn) } }, [])

  useEffect(() => {
    if (compact || historyLoaded) return
    ;(async () => {
      try {
        const files = await invoke<HistoryFile[]>('read_logs', { maxFiles: 5 })
        setHistoryFiles(files)
      } catch (_) {}
      setHistoryLoaded(true)
    })()
  }, [historyLoaded])

  const currentReversed = [...logs].reverse().slice(0, compact ? 50 : 200)

  // Full-card mode (Log tab): each file is a standalone card like SettingsCard
  if (!compact) {
    return (
      <div className="flex-1 overflow-y-auto p-6 space-y-3">
        {/* Current session card */}
        <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
          <div className="flex items-center justify-between px-3 py-2">
            <div className="flex items-center gap-2">
              <FileText className="w-4 h-4 text-accent" />
              <span className="text-sm font-medium text-text-primary">Current Session</span>
              <span className="text-xs text-text-muted">({currentReversed.length})</span>
            </div>
            <Tooltip text="清空当前日志"><button onClick={() => { gLogs = []; gLogListeners.forEach(f => f()) }}
              className="p-1 rounded-md text-text-secondary hover:text-error hover:bg-bg-tertiary transition-colors"><Trash2 className="w-3.5 h-3.5" /></button></Tooltip>
          </div>
          <div className="border-t border-border" />
          <div className="max-h-[400px] overflow-y-auto p-4 font-mono text-xs space-y-0.5">
            {currentReversed.length === 0
              ? <div className="text-text-muted text-center py-4">No current logs</div>
              : currentReversed.map((l, i) => (
                  <div key={`cur-${i}`} className="text-text-secondary"><span className="text-text-muted">[{l.ts}]</span> {l.msg}</div>
                ))
            }
          </div>
        </div>

        {/* History file cards */}
        {historyFiles.map((f, fi) => {
          const open = openFiles.has(fi)
          return (
            <div key={f.name} className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
              <div role="button" tabIndex={0} onClick={() => { const s = new Set(openFiles); open ? s.delete(fi) : s.add(fi); setOpenFiles(s) }}
                onKeyDown={e=>{if(e.key==='Enter'||e.key===' '){(e.currentTarget as HTMLElement).click()}}}
                className="w-full flex items-center justify-between px-3 py-2 hover:bg-bg-hover cursor-pointer transition-colors outline-none">
                <div className="flex items-center gap-2 min-w-0">
                  <FileText className="w-4 h-4 text-text-muted shrink-0" />
                  <span className="text-sm font-medium text-text-primary truncate">{f.name}</span>
                  <span className="text-xs text-text-muted shrink-0">{f.lines.length} lines</span>
                </div>
                <ChevronDown className={`w-4 h-4 text-text-muted transition-transform duration-150 shrink-0 ${open?'rotate-180':''}`} />
              </div>
              {open && (
                <>
                  <div className="border-t border-border" />
                  <div className="max-h-[400px] overflow-y-auto p-4 font-mono text-xs space-y-0.5">
                    {f.lines.map((l, i) => (
                      <div key={i} className="text-text-muted">{l}</div>
                    ))}
                  </div>
                </>
              )}
            </div>
          )
        })}
        {currentReversed.length === 0 && historyFiles.length === 0 && (
          <div className="flex items-center justify-center py-12 text-sm text-text-muted">No logs yet</div>
        )}
      </div>
    )
  }

  // Compact mode (right sidebar): simple current-session log feed
  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden flex flex-col min-h-0">
      <div role="button" tabIndex={0} onClick={() => setExpanded(!expanded)} onKeyDown={e=>{if(e.key==='Enter'||e.key===' '){(e.currentTarget as HTMLElement).click()}}}
        className="w-full flex items-center justify-between px-3 py-2 hover:bg-bg-hover cursor-pointer transition-colors outline-none shrink-0">
        <div className="flex items-center gap-2">
          <FileText className="w-4 h-4 text-text-secondary" />
          <span className="text-sm font-medium text-text-primary">Log</span>
          <span className="text-xs text-text-muted">{currentReversed.length}</span>
        </div>
        <div className="flex items-center gap-0.5">
          <Tooltip text="清空日志">
            <button onClick={e => { e.stopPropagation(); gLogs = []; gLogListeners.forEach(f => f()) }}
              className="p-1 rounded-md text-text-secondary hover:text-error hover:bg-bg-tertiary transition-colors" onClick={e => { e.stopPropagation(); addLog('Action: clear logs'); gLogs = []; gLogListeners.forEach(f => f()) }}>
              <Trash2 className="w-3.5 h-3.5" />
          </Tooltip>
          <ChevronDown className={`w-4 h-4 text-text-muted transition-transform duration-150 ${expanded?'rotate-180':''}`} />
        </div>
      </div>
      <div className="grid transition-[grid-template-rows] duration-150 ease-out flex-1 min-h-0"
        style={{ gridTemplateRows: expanded ? '1fr' : '0fr' }}>
        <div className="overflow-hidden min-h-0">
          <div className="border-t border-border" />
          <div className="h-[180px] overflow-y-auto p-4 flex flex-col">
            {currentReversed.length === 0 ? (
              <div className="flex items-center justify-center h-full text-sm text-text-muted">No logs</div>
            ) : (
              <div className="space-y-1 font-mono text-xs text-text-secondary pt-1">
                {currentReversed.slice(0, 100).map((l, i) => (
                  <div key={i}><span className="text-text-muted">[{l.ts}]</span> {l.msg}</div>
                ))}
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
      <div role="button" tabIndex={0} onClick={() => setExpanded(!expanded)} onKeyDown={e=>{if(e.key==='Enter'||e.key===' '){(e.currentTarget as HTMLElement).click()}}}
        className="w-full flex items-center justify-between px-3 py-2 hover:bg-bg-hover cursor-pointer transition-colors outline-none">
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

// ═══ Settings Page ═══
function SettingsPage({ forceMethod, setForceMethod }: { forceMethod: string; setForceMethod: (m: string) => void }) {
  const colors = ['#3B82F6','#8B5CF6','#EC4899','#F59E0B','#10B981','#EF4444']
  const [accent, setAccent] = useState('#3B82F6')
  const methods = [
    { v: 'auto',          l: 'Auto (auto-detect)' },
    { v: 'wgc',           l: 'WGC (GPU FramePool)' },
    { v: 'dxgi',          l: 'DXGI (Desktop Duplication)' },
    { v: 'gdi',           l: 'GDI (GetWindowDC)' },
    { v: 'printwindow',   l: 'PrintWindow' },
    { v: 'screenbitblt',  l: 'Screen BitBlt' },
  ]
  return (
    <div className="flex-1 overflow-y-auto p-6 space-y-3">
      <SettingsCard icon={<MonitorUp className="w-4 h-4 text-text-secondary" />} title="Connection">
        <div className="flex items-center gap-3 mb-2"><label className="text-sm text-text-secondary w-28 shrink-0">Window Title</label><Tooltip text="要捕获的游戏窗口标题"><input defaultValue="Tic Tac Toe" className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
        <div className="flex items-center gap-3 mb-2"><label className="text-sm text-text-secondary w-28 shrink-0">Server Host</label><Tooltip text="AI模型服务器地址"><input defaultValue="127.0.0.1" className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
        <div className="flex items-center gap-3 mb-2"><label className="text-sm text-text-secondary w-28 shrink-0">Server Port</label><Tooltip text="AI模型服务端口"><input defaultValue="9999" className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
        <div className="flex items-center gap-3 mb-2"><label className="text-sm text-text-secondary w-28 shrink-0">Capture FPS</label><Tooltip text="截屏预览帧率"><input defaultValue="20" className="w-20 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
        <div className="flex items-center gap-3"><label className="text-sm text-text-secondary w-28 shrink-0">Method</label><Tooltip text="显式选择捕获技术方案，选 Auto 则自动回退"><select value={forceMethod} onChange={e => { setForceMethod(e.target.value); addLog(`Setting: capture method = ${e.target.value}`) }} className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent cursor-pointer">{methods.map(m => <option key={m.v} value={m.v}>{m.l}</option>)}</select></Tooltip></div>
      </SettingsCard>

      <SettingsCard icon={<Sun className="w-4 h-4 text-text-secondary" />} title="Theme">
        <div className="flex items-center gap-2 mb-2">
          <label className="text-sm text-text-secondary w-28 shrink-0">Mode</label>
          <div className="flex gap-1">
            {[['Light','light'],['Dark','dark'],['System','system']].map(([l,v])=>
              <button key={v} onClick={()=>document.documentElement.classList.toggle('dark',v==='dark')}
                className="px-3 py-1 rounded-full text-xs font-medium bg-bg-tertiary text-text-secondary hover:bg-bg-hover transition-colors">{l}</button>
            )}
          </div>
        </div>
        <div className="flex items-center gap-2">
          <label className="text-sm text-text-secondary w-28 shrink-0">Accent</label>
          <div className="flex gap-1.5">
            {colors.map(c=>(
              <button key={c} onClick={()=>setAccent(c)}
                className="w-6 h-6 rounded-full border-2 transition-all" style={{background:c,borderColor:accent===c?'white':'transparent'}} />
            ))}
          </div>
        </div>
      </SettingsCard>

      <SettingsCard icon={<Settings className="w-4 h-4 text-text-secondary" />} title="Model Context" defaultExpanded={false}>
        <div className="text-xs text-text-muted mb-2">Base model + fine-tuning adapter for specific games.</div>
        <div className="flex items-center gap-3 mb-2"><label className="text-sm text-text-secondary w-28 shrink-0">Base Model</label><Tooltip text="基础视觉模型"><input defaultValue="GenericAgent v1" className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
        <div className="flex items-center gap-3"><label className="text-sm text-text-secondary w-28 shrink-0">Adapter</label><Tooltip text="游戏微调权重"><input defaultValue="tictactoe-finetune" className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
      </SettingsCard>

      <SettingsCard icon={<RefreshCw className="w-4 h-4 text-text-secondary" />} title="Update">
        <div className="flex items-center justify-between">
          <div><div className="text-sm text-text-secondary">Version v0.1.0</div><div className="text-xs text-text-muted">Latest version</div></div>
          <ActionBtn icon={<Settings className="w-3.5 h-3.5" />} label="Check" title="检查新版本" variant="outline" />
        </div>
      </SettingsCard>

      <SettingsCard icon={<FileText className="w-4 h-4 text-text-secondary" />} title="Log">
        <div className="flex items-center gap-3 mb-2"><label className="text-sm text-text-secondary w-28 shrink-0">Directory</label><Tooltip text="日志文件存放路径"><input defaultValue="logs/" className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
        <div className="flex items-center gap-3"><label className="text-sm text-text-secondary w-28 shrink-0">Keep Files</label><Tooltip text="最多保留日志文件数"><select defaultValue="5" className="h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent">{[3,5,7,10].map(n=><option key={n}>{n} files</option>)}</select></Tooltip></div>
      </SettingsCard>

      <SettingsCard icon={<Monitor className="w-4 h-4 text-text-secondary" />} title="Project">
        <div className="text-xs text-text-muted mb-3">If this project helps you, please star!</div>
        <ActionBtn icon={<span>★</span>} label="Star on GitHub" title="给项目点Star支持开发" variant="primary" />
        <div className="mt-4 pt-4 border-t border-border">
          <div className="text-xs font-medium text-text-secondary mb-1">Links</div>
          {[{l:'GitHub',u:'https://github.com/Andyqwe44/tictactoe'},{l:'Slint',u:'https://slint.dev'},{l:'Tauri 2',u:'https://v2.tauri.app'}].map(x=>
          <button key={x.l} onClick={()=>{try{window.open(x.u,'_blank')}catch{}}}
            className="block text-sm text-accent hover:underline py-0.5 cursor-pointer">{x.l}</button>
        )}
          <div className="text-xs font-medium text-text-secondary mt-3 mb-1">Credits</div>
          <div className="text-xs text-text-muted">Andyqwe44 · Tauri 2 · React · Tailwind · DXGI · Interception · PyTorch</div>
        </div>
      </SettingsCard>
      <div className="h-4" />
    </div>
  )
}

function DashboardView() {
  const [info, setInfo] = useState({ appVer: 'v0.1.0', resVer: '-', screen: '?x?', status: 'Idle', uptime: '0s' })
  useEffect(() => {
    (async () => {
      try {
        const si = await invoke<{w:number;h:number}>('screen_info')
        setInfo(i => ({ ...i, screen: `${si.w}×${si.h}`, status: 'Ready' }))
      } catch (_) {}
    })()
  }, [])
  return (
    <div className="flex-1 overflow-y-auto p-6 space-y-3">
      <SettingsCard icon={<Monitor className="w-4 h-4 text-text-secondary" />} title="System">
        <div className="space-y-2 text-sm">
          <div className="flex justify-between"><span className="text-text-secondary">App Version</span><span className="text-text-primary">{info.appVer}</span></div>
          <div className="flex justify-between"><span className="text-text-secondary">Resource Version</span><span className="text-text-primary">{info.resVer}</span></div>
          <div className="flex justify-between"><span className="text-text-secondary">Screen Resolution</span><span className="text-text-primary font-mono">{info.screen}</span></div>
          <div className="flex justify-between"><span className="text-text-secondary">Service Status</span><span className="text-success">{info.status}</span></div>
        </div>
      </SettingsCard>
      <SettingsCard icon={<FileText className="w-4 h-4 text-text-secondary" />} title="Capture Pipeline">
        <div className="space-y-2 text-sm">
          <div className="flex justify-between"><span className="text-text-secondary">Window Capture</span><span className="text-accent">WGC FramePool</span></div>
          <div className="flex justify-between"><span className="text-text-secondary">Desktop Capture</span><span className="text-text-primary">DXGI Desktop Dup</span></div>
          <div className="flex justify-between"><span className="text-text-secondary">Fallback</span><span className="text-text-muted">GDI BitBlt</span></div>
          <div className="flex justify-between"><span className="text-text-secondary">Encoding</span><span className="text-text-primary">Raw RGBA (Canvas)</span></div>
        </div>
      </SettingsCard>
      <SettingsCard icon={<RefreshCw className="w-4 h-4 text-text-secondary" />} title="Update">
        <div className="space-y-3">
          <div className="flex items-center justify-between">
            <div><div className="text-sm text-text-primary">Current: {info.appVer}</div><div className="text-xs text-text-muted">Check for new versions</div></div>
            <ActionBtn icon={<RefreshCw className="w-3.5 h-3.5" />} label="Check" title="检查更新" variant="outline" />
          </div>
          <div className="border-t border-border pt-2">
            <div className="flex items-center gap-3">
              <label className="text-sm text-text-secondary w-20 shrink-0">Source</label>
              <select defaultValue="github" className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent">
                <option value="github">GitHub Releases</option>
                <option value="gitee">Gitee Mirror</option>
                <option value="local">Local File</option>
              </select>
            </div>
          </div>
        </div>
      </SettingsCard>
      <SettingsCard icon={<MonitorUp className="w-4 h-4 text-text-secondary" />} title="Resources">
        <div className="space-y-2 text-sm">
          <div className="flex justify-between"><span className="text-text-secondary">Log Directory</span><span className="text-text-primary font-mono text-xs">log/</span></div>
          <div className="flex justify-between"><span className="text-text-secondary">Capture Backend</span><span className="text-accent">WGC + DXGI</span></div>
          <div className="flex justify-between"><span className="text-text-secondary">Transport</span><span className="text-text-primary">TCP :9999</span></div>
          <div className="flex justify-between"><span className="text-text-secondary">UI Framework</span><span className="text-text-muted">Tauri 2 + React + Tailwind</span></div>
        </div>
      </SettingsCard>
      <div className="h-4" />
    </div>
  )
}

export default function App() {
  const [tab, setTab] = useState('Dashboard')
  const [running, setRunning] = useState(false)
  const [rightWidth, setRightWidth] = useState(DEFAULT_RIGHT_WIDTH)
  const [rightCollapsed, setRightCollapsed] = useState(false)
  const isResizing = useRef(false)
  const [selWindow, setSelWindow] = useState<WindowInfo>({ title: ' Entire Desktop', category: 'desktop', hwnd: 0 })
  const [screenRatio, setScreenRatio] = useState(16/9)
  const [forceMethod, setForceMethod] = useState('auto')

  useEffect(() => {
    (async () => {
      try {
        const si = await invoke<{w:number;h:number}>('screen_info')
        setScreenRatio(si.w / si.h)
      } catch (_) {}
    })()
  }, [])

  // Yellow border overlay on selected window
  useEffect(() => {
    (async () => {
      try {
        await invoke('highlight_window', { hwnd: selWindow.hwnd })
      } catch (_) {}
    })()
    return () => {
      (async () => {
        try {
          const { invoke } = await import('@tauri-apps/api/core')
          await invoke('highlight_window', { hwnd: 0 })
        } catch (_) {}
      })()
    }
  }, [selWindow.hwnd])

  const handleResizeStart = useCallback((e: React.MouseEvent) => {
    e.preventDefault(); isResizing.current = true
    document.body.style.cursor = 'col-resize'; document.body.style.userSelect = 'none'
    const onMove = (ev: MouseEvent) => {
      const w = document.body.clientWidth - ev.clientX
      if (w < 160) setRightCollapsed(true)
      else { setRightCollapsed(false); setRightWidth(Math.max(324, Math.min(400, w))) }
    }
    const onUp = () => { isResizing.current = false; document.body.style.cursor = ''; document.body.style.userSelect = ''; window.removeEventListener('mousemove', onMove); window.removeEventListener('mouseup', onUp) }
    window.addEventListener('mousemove', onMove); window.addEventListener('mouseup', onUp)
  }, [])

  return (
    <div className="h-full flex flex-col bg-bg-primary">
      <TopBar tab={tab} setTab={setTab} running={running}
        onStart={() => setRunning(true)} onStop={() => setRunning(false)} />
      <div className="flex-1 flex overflow-hidden">
        <div className="flex-1 flex flex-col overflow-hidden border-r border-border" style={{ minWidth: MIN_LEFT_WIDTH }}>
          {tab === 'Dashboard' && <DashboardView />}
          {tab === 'Monitor' && (
            <div className="flex-1 flex items-center justify-center p-6">
              <div className="rounded-xl bg-bg-secondary p-8 text-center space-y-3 max-w-md w-full">
                <div className="text-5xl opacity-20">🎮</div>
                <div className="text-sm text-text-secondary">{running ? 'Task running...' : 'Press Start to begin the agent'}</div>
                <div className="text-xs text-text-muted">Target: {selWindow.title}</div>
              </div>
            </div>
          )}
          {tab === 'Log' && <div className="flex-1"><LogPanel /></div>}
          {tab === 'Settings' && <SettingsPage forceMethod={forceMethod} setForceMethod={setForceMethod} />}
          <BottomBar running={running} fps={0} lat={0} />
        </div>
        <Tooltip text={rightCollapsed ? "向右拖拽展开面板" : "拖拽调整面板宽度，向右拖到底可折叠"}>
          <div onMouseDown={handleResizeStart}
            className={`${rightCollapsed ? 'w-4' : 'w-1'} hover:bg-accent/50 cursor-col-resize flex items-center justify-center group shrink-0 transition-all select-none bg-transparent`}>
            <div className="w-[2px] h-8 rounded-full transition-colors bg-border group-hover:bg-accent" />
          </div>
        </Tooltip>
        {!rightCollapsed && (
          <div className="flex flex-col p-3 gap-3 overflow-y-auto shrink-0" style={{ width: rightWidth, minWidth: 324, maxWidth: 400 }}>
            <ConnectionPanel onSelect={setSelWindow} onDisconnect={() => {
              setSelWindow({ title: ' Entire Desktop', category: 'desktop', hwnd: 0 })
              addLog('Disconnected, back to desktop')
            }} />
            <LogPanel compact />
            <ScreenshotPanel selWin={selWindow} screenRatio={screenRatio} forceMethod={forceMethod} setForceMethod={setForceMethod} />
          </div>
        )}
      </div>
    </div>
  )
}
