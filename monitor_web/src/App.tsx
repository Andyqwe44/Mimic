import { useState, useEffect, useRef, useCallback } from 'react'
import { createPortal } from 'react-dom'
import { Play, Square, Camera, Monitor, Settings, Moon, Sun, ChevronUp, ChevronDown, FileText, Trash2, X, MonitorUp, Search, MonitorSmartphone, RefreshCw } from 'lucide-react'

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
const DEFAULT_RIGHT_WIDTH = 340

// ═══ Shared log state ───
type LogEntry = { ts: string; msg: string }
let gLogs: LogEntry[] = []
let gLogListeners: (() => void)[] = []
function addLog(msg: string) { gLogs = [...gLogs, { ts: new Date().toLocaleTimeString(), msg }]; gLogListeners.forEach(f => f()) }

// ═══ Theme btn ───
function ThemeBtn() {
  const [dark, setDark] = useState(false)
  return (
    <Tooltip text={dark ? "切换亮色主题" : "切换暗色主题"}>
      <button onClick={() => { setDark(!dark); document.documentElement.classList.toggle('dark', !dark) }}
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

// ═══ TopBar ───
function TopBar({ tab, setTab, running, onStart, onStop }: {
  tab: string; setTab: (t: 'Monitor'|'Log'|'Config'|'Settings') => void; running: boolean; onStart: () => void; onStop: () => void
}) {
  const tabs = ['Monitor', 'Log'] as const
  return (
    <div className="flex items-center h-10 bg-bg-secondary border-b border-border select-none shrink-0">
      <div className="flex-1 flex items-center h-full overflow-x-auto px-1">
        {tabs.map(t => (
          <Tooltip key={t} text={`切换到 ${t} 面板`}>
            <button onClick={() => setTab(t)}
              className={`group flex items-center h-full px-3 cursor-pointer border-r border-border min-w-[90px] transition-colors
                ${t === tab ? 'bg-bg-primary text-accent border-b-2 border-b-accent' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover border-b-2 border-b-transparent'}`}>
              <span className="flex-1 truncate text-sm">{t}</span>
            </button>
          </Tooltip>
        ))}
      </div>
      <div className="flex items-center gap-1 px-2">
        {running
          ? <ActionBtn icon={<Square className="w-3.5 h-3.5" />} label="Stop" title="停止所有运行中的任务" variant="danger" onClick={onStop} />
          : <ActionBtn icon={<Play className="w-3.5 h-3.5" />} label="Start" title="启动agent任务" variant="primary" onClick={onStart} />
        }
        <div className="mx-1 h-4 w-px bg-border" />
        <ThemeBtn />
        <IconBtn title="设置" icon={<Settings className="w-4 h-4" />} onClick={() => setTab('Settings')} />
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
      const { invoke } = await import('@tauri-apps/api/core')
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
      const { invoke } = await import('@tauri-apps/api/core')
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
function ConnectionPanel({ onSelect }: { onSelect: (w: WindowInfo) => void }) {
  const [pickerOpen, setPickerOpen] = useState(false)
  const [title, setTitle] = useState(' Entire Desktop')
  const [ip, setIp] = useState('127.0.0.1')
  const [port, setPort] = useState('9999')
  const handleSelect = (w: WindowInfo) => { setTitle(w.title); onSelect(w); addLog(`Selected: ${w.title}`) }

  // Auto-split "IP::port" — detect :: in either field, distribute to both
  const handleIpChange = (value: string) => {
    if (value.includes('::')) {
      const [a, b] = value.split('::', 2)
      setIp(a.trim())
      if (b?.trim()) setPort(b.trim())
    } else {
      setIp(value)
    }
  }
  const handlePortChange = (value: string) => {
    if (value.includes('::')) {
      const [a, b] = value.split('::', 2)
      if (a?.trim()) setIp(a.trim())
      setPort(b?.trim() ?? '')
    } else {
      setPort(value)
    }
  }

  return (
    <div className="rounded-xl bg-bg-secondary p-4">
      <div className="flex items-center justify-between mb-2">
        <span className="text-xs font-medium text-text-primary ml-1">Connection</span>
      </div>
      <div className="space-y-2">
        <div className="flex gap-2">
          <Tooltip text="要捕获的游戏窗口标题">
            <input value={title} onChange={e => setTitle(e.target.value)} placeholder="Window Title"
              className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm text-text-primary outline-none focus:border-accent transition-colors placeholder:text-text-muted" />
          </Tooltip>
          <ActionBtn icon={<MonitorUp className="w-3.5 h-3.5" />} label="Select" title="选择要捕获的窗口或桌面" variant="primary" onClick={() => setPickerOpen(true)} />
        </div>
        <div className="flex gap-2">
          <Tooltip text="AI模型服务器IP地址">
            <input value={ip} onChange={e => handleIpChange(e.target.value)} placeholder="IP Address"
              className="flex-1 min-w-0 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm text-text-primary outline-none focus:border-accent transition-colors placeholder:text-text-muted" />
          </Tooltip>
          <Tooltip text="Port端口号">
            <input value={port} onChange={e => handlePortChange(e.target.value)} placeholder="Port"
              className="w-20 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm text-text-primary outline-none focus:border-accent transition-colors placeholder:text-text-muted" />
          </Tooltip>
        </div>
      </div>
      <WindowPickerModal open={pickerOpen} onClose={() => setPickerOpen(false)} onSelect={handleSelect} />
    </div>
  )
}

// ═══ Screenshot Panel ───
function ScreenshotPanel({ selWin }: { selWin?: WindowInfo }) {
  const [expanded, setExpanded] = useState(true)
  const [previewing, setPreviewing] = useState(false)
  const [imgSrc, setImgSrc] = useState('')       // single-frame <img> base64
  const [fps, setFps] = useState(0)
  const [capMethod, setCapMethod] = useState('')
  const previewingRef = useRef(false)
  const framesRef = useRef(0)
  const lastFpsRef = useRef(Date.now())
  const videoRef = useRef<HTMLVideoElement>(null)
  const msRef = useRef<MediaSource | null>(null)
  const sbRef = useRef<SourceBuffer | null>(null)
  const initDataRef = useRef<ArrayBuffer | null>(null)
  const codecRef = useRef('avc1.42C01E')
  const pendingSegments = useRef<ArrayBuffer[]>([])
  const unlistenInitRef = useRef<(() => void) | null>(null)
  const unlistenTickRef = useRef<(() => void) | null>(null)

  // ── H.264 Preview: MSE + <video> ──
  const togglePreview = async () => {
    if (previewing) {
      // Stop
      previewingRef.current = false; setPreviewing(false); setFps(0); setCapMethod('')
      if (unlistenTickRef.current) { unlistenTickRef.current(); unlistenTickRef.current = null }
      if (unlistenInitRef.current) { unlistenInitRef.current(); unlistenInitRef.current = null }
      try { const { invoke } = await import('@tauri-apps/api/core'); await invoke<string>('h264_stream_stop') } catch (_) {}
      // Close MediaSource
      if (msRef.current && msRef.current.readyState === 'open') {
        try { msRef.current.endOfStream() } catch (_) {}
      }
      sbRef.current = null; msRef.current = null
      initDataRef.current = null; pendingSegments.current = []
      if (videoRef.current) videoRef.current.src = ''
      addLog('Preview stopped')
    } else {
      // Start
      const hwnd = selWin?.hwnd ?? 0
      addLog(`H.264 Preview starting: ${selWin?.title ?? 'desktop'}`)
      try { const { invoke } = await import('@tauri-apps/api/core'); await invoke<string>('h264_stream_start', { hwnd }) }
      catch (e) { addLog(`H.264 start failed: ${e}`); return }

      previewingRef.current = true; setPreviewing(true)
      initDataRef.current = null; pendingSegments.current = []
      framesRef.current = 0; lastFpsRef.current = Date.now()

      // Create MediaSource
      const ms = new MediaSource()
      msRef.current = ms
      const url = URL.createObjectURL(ms)
      if (videoRef.current) {
        videoRef.current.src = url
        videoRef.current.play().catch(() => {}) // autoplay
      }

      ms.addEventListener('sourceopen', () => {
        sbRef.current = null
        if (!previewingRef.current) return

        // Try actual codec first, fallback to baseline
        let mime = `video/mp4; codecs="${codecRef.current}"`
        if (!MediaSource.isTypeSupported(mime)) {
          mime = 'video/mp4; codecs="avc1.42C01E"'
          if (!MediaSource.isTypeSupported(mime)) {
            addLog('MSE: no supported H.264 codec')
            return
          }
        }
        addLog(`MSE: codec=${mime}`)
        try {
          const sb = ms.addSourceBuffer(mime)
          sb.mode = 'sequence'
          sb.addEventListener('error', () => addLog('SourceBuffer error'))
          sbRef.current = sb

          // Append init segment if ready
          if (initDataRef.current) {
            try { sb.appendBuffer(initDataRef.current) } catch (e) { addLog(`Init append error: ${e}`) }
            initDataRef.current = null
          }

          // Append any pending segments
          for (const seg of pendingSegments.current) {
            try { if (!sb.updating) sb.appendBuffer(seg) }
            catch (e) { addLog(`Pending append error: ${e}`) }
          }
          pendingSegments.current = []

          sb.addEventListener('updateend', () => {
            // Flush pending segments when buffer completes
            if (pendingSegments.current.length > 0 && sbRef.current && !sbRef.current.updating) {
              const next = pendingSegments.current.shift()!
              try { sbRef.current.appendBuffer(next) } catch (_) {}
            }
            // Trim buffer to last 2 seconds
            if (sbRef.current && ms.readyState === 'open' && ms.duration > 4) {
              const removeEnd = ms.duration - 2
              if (removeEnd > 0) {
                try { sbRef.current.remove(0, removeEnd) } catch (_) {}
              }
            }
          })
        } catch (e) {
          addLog(`MSE addSourceBuffer failed: ${e}`)
        }
      })

      // Listen for init event
      const { listen } = await import('@tauri-apps/api/event')
      const unlistenInit = await listen<{ data: string; codec: string; method: string }>('h264-init', (event) => {
        if (!previewingRef.current) return
        setCapMethod(event.payload.method)
        if (event.payload.codec) codecRef.current = event.payload.codec
        const bytes = base64ToBuf(event.payload.data)
        initDataRef.current = bytes
        // Try to append if sourcebuffer is ready
        if (sbRef.current && !sbRef.current.updating) {
          try { sbRef.current.appendBuffer(bytes); initDataRef.current = null }
          catch (_) { addLog('Init append on event error') }
        }
      })
      unlistenInitRef.current = unlistenInit

      // Listen for tick events
      const unlistenTick = await listen('h264-tick', async () => {
        if (!previewingRef.current) return
        try {
          const { invoke } = await import('@tauri-apps/api/core')
          const b64 = await invoke<string>('h264_poll')
          if (b64) {
            const bytes = base64ToBuf(b64)
            const sb = sbRef.current
            if (sb && !sb.updating) {
              try { sb.appendBuffer(bytes) }
              catch (_) {
                // Queue for later
                pendingSegments.current.push(bytes)
              }
            } else {
              pendingSegments.current.push(bytes)
            }
          }
        } catch (_) {}
        framesRef.current++
        const now = Date.now(); const elapsed = now - lastFpsRef.current
        if (elapsed >= 1000) {
          setFps(Math.round(framesRef.current * 1000 / elapsed))
          framesRef.current = 0; lastFpsRef.current = now
        }
      })
      unlistenTickRef.current = unlistenTick
    }
  }

  // Cleanup on unmount
  useEffect(() => { return () => {
    previewingRef.current = false
    if (unlistenInitRef.current) unlistenInitRef.current()
    if (unlistenTickRef.current) unlistenTickRef.current()
  } }, [])

  return (
    <div className="mt-3 rounded-xl bg-bg-secondary p-4">
      <div className="flex items-center justify-between mb-2">
        <span className="text-xs font-medium text-text-primary ml-1">Screenshot</span>
        <div className="flex items-center gap-1">
          <IconBtn title="单帧截图" icon={<Camera className="w-3.5 h-3.5" />}
            onClick={async () => {
              const hwnd = selWin?.hwnd ?? 0;
              addLog(`Capturing ${hwnd ? 'window hwnd='+hwnd : 'desktop'}...`)
              const t0 = Date.now()
              try {
                const { invoke } = await import('@tauri-apps/api/core')
                const b64 = await invoke<string>('capture_window', { hwnd })
                const elapsed = Date.now() - t0
                setImgSrc(`data:image/png;base64,${b64}`)
                addLog(`Screenshot OK: ${b64.length} chars base64, ${elapsed}ms total`)
              } catch { addLog(`Screenshot failed after ${Date.now() - t0}ms`) }
            }} />
          {previewing
            ? <ActionBtn icon={<Square className="w-3 h-3" />} label="Stop" title="停止截屏预览 (H.264)" variant="danger" onClick={togglePreview} />
            : <ActionBtn icon={<Play className="w-3 h-3" />} label="Preview" title="开始实时截屏预览 (H.264 60FPS GPU)" variant="primary" onClick={togglePreview} />
          }
          <IconBtn title={expanded ? "折叠截图面板" : "展开截图面板"}
            icon={expanded ? <ChevronUp className="w-3.5 h-3.5" /> : <ChevronDown className="w-3.5 h-3.5" />}
            onClick={() => setExpanded(!expanded)} />
        </div>
      </div>
      {expanded && (
        <div className="min-h-[160px] rounded-lg bg-bg-primary overflow-hidden flex items-center justify-center relative">
          {previewing ? (
            <video ref={videoRef} className="w-full h-auto object-contain" muted playsInline />
          ) : imgSrc ? (
            <img src={imgSrc} className="w-full h-auto object-contain" alt="preview" />
          ) : (
            <span className="text-sm text-text-muted">Press Preview</span>
          )}
          {previewing && (
            <div className="absolute bottom-2 right-2 flex items-center gap-1.5 text-xs bg-bg-primary/80 px-2 py-0.5 rounded font-mono">
              {capMethod && <span className="text-success">{capMethod}</span>}
              {capMethod && <span className="text-border">|</span>}
              <span className="text-accent">{fps} FPS</span>
            </div>
          )}
        </div>
      )}
    </div>
  )
}

// Base64 → ArrayBuffer helper (for MSE SourceBuffer.appendBuffer)
function base64ToBuf(b64: string): ArrayBuffer {
  const binary = atob(b64)
  const bytes = new Uint8Array(binary.length)
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i)
  return bytes.buffer as ArrayBuffer
}

// ═══ Log Panel ───
function LogPanel() {
  const [expanded, setExpanded] = useState(true)
  const [logs, setLogs] = useState(gLogs)
  useEffect(() => { const fn = () => setLogs([...gLogs]); gLogListeners.push(fn); return () => { gLogListeners = gLogListeners.filter(f => f !== fn) } }, [])
  // Newest first
  const reversed = [...logs].reverse()
  return (
    <div className="mt-3 rounded-xl bg-bg-secondary p-4">
      <div className="flex items-center justify-between mb-2">
        <span className="text-xs font-medium text-text-primary ml-1">Log</span>
        <div className="flex items-center gap-1">
          <IconBtn title="运行日志" icon={<FileText className="w-3.5 h-3.5" />} />
          <IconBtn title="清空日志" icon={<Trash2 className="w-3.5 h-3.5" />} onClick={() => { gLogs = []; gLogListeners.forEach(f => f()) }} />
          <IconBtn icon={expanded ? <ChevronUp className="w-3.5 h-3.5" /> : <ChevronDown className="w-3.5 h-3.5" />} title={expanded ? "折叠" : "展开"} onClick={() => setExpanded(!expanded)} />
        </div>
      </div>
      {expanded && (
        <div className="h-[200px] overflow-y-scroll rounded-lg bg-bg-primary p-3">
          {reversed.length === 0 ? (
            <div className="flex items-center justify-center h-full text-sm text-text-muted">No logs</div>
          ) : (
            <div className="space-y-1 font-mono text-xs text-text-secondary">
              {reversed.map((l, i) => (
                <div key={i}><span className="text-text-muted">[{l.ts}]</span> {l.msg}</div>
              ))}
            </div>
          )}
        </div>
      )}
    </div>
  )
}

// ═══ App ───
// ═══ Settings Page (includes Config, Theme, Update, Log, Links, Credits) ═══
function SettingsPage() {
  const colors = ['#3B82F6','#8B5CF6','#EC4899','#F59E0B','#10B981','#EF4444']
  const [accent, setAccent] = useState('#3B82F6')
  return (
    <div className="flex-1 overflow-y-auto p-6 space-y-4">
      {/* Game & Server (merged from Config) */}
      <div className="rounded-xl bg-bg-secondary p-4">
        <div className="text-sm font-semibold mb-3">Connection</div>
        <div className="flex items-center gap-3 mb-2"><label className="text-sm text-text-secondary w-28 shrink-0">Window Title</label><Tooltip text="要捕获的游戏窗口标题"><input defaultValue="Tic Tac Toe" className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
        <div className="flex items-center gap-3 mb-2"><label className="text-sm text-text-secondary w-28 shrink-0">Server Host</label><Tooltip text="AI模型服务器地址"><input defaultValue="127.0.0.1" className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
        <div className="flex items-center gap-3 mb-2"><label className="text-sm text-text-secondary w-28 shrink-0">Server Port</label><Tooltip text="AI模型服务端口"><input defaultValue="9999" className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
        <div className="flex items-center gap-3"><label className="text-sm text-text-secondary w-28 shrink-0">Capture FPS</label><Tooltip text="截屏预览帧率"><input defaultValue="20" className="w-20 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
      </div>

      {/* Theme */}
      <div className="rounded-xl bg-bg-secondary p-4">
        <div className="text-sm font-semibold mb-3">Theme</div>
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
      </div>

      {/* Model */}
      <div className="rounded-xl bg-bg-secondary p-4">
        <div className="text-sm font-semibold mb-3">Model Context</div>
        <div className="text-xs text-text-muted mb-2">Base model + fine-tuning adapter for specific games.</div>
        <div className="flex items-center gap-3 mb-2"><label className="text-sm text-text-secondary w-28 shrink-0">Base Model</label><Tooltip text="基础视觉模型"><input defaultValue="GenericAgent v1" className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
        <div className="flex items-center gap-3"><label className="text-sm text-text-secondary w-28 shrink-0">Adapter</label><Tooltip text="游戏微调权重"><input defaultValue="tictactoe-finetune" className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
      </div>

      {/* Update */}
      <div className="rounded-xl bg-bg-secondary p-4">
        <div className="text-sm font-semibold mb-3">Update</div>
        <div className="flex items-center justify-between">
          <div><div className="text-sm text-text-secondary">Version v0.1.0</div><div className="text-xs text-text-muted">Latest version</div></div>
          <ActionBtn icon={<Settings className="w-3.5 h-3.5" />} label="Check" title="检查新版本" variant="outline" />
        </div>
      </div>

      {/* Log */}
      <div className="rounded-xl bg-bg-secondary p-4">
        <div className="text-sm font-semibold mb-3">Log</div>
        <div className="flex items-center gap-3 mb-2"><label className="text-sm text-text-secondary w-28 shrink-0">Directory</label><Tooltip text="日志文件存放路径"><input defaultValue="logs/" className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent" /></Tooltip></div>
        <div className="flex items-center gap-3"><label className="text-sm text-text-secondary w-28 shrink-0">Keep Files</label><Tooltip text="最多保留日志文件数"><select defaultValue="5" className="h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent">{[3,5,7,10].map(n=><option key={n}>{n} files</option>)}</select></Tooltip></div>
      </div>

      {/* Links + Credits */}
      <div className="rounded-xl bg-bg-secondary p-4">
        <div className="text-sm font-semibold mb-3">Project</div>
        <div className="text-xs text-text-muted mb-3">If this project helps you, please star!</div>
        <ActionBtn icon={<span>★</span>} label="Star on GitHub" title="给项目点Star支持开发" variant="primary" />
        <div className="mt-4 pt-4 border-t border-border">
          <div className="text-xs font-medium text-text-secondary mb-1">Links</div>
          {[{l:'GitHub',u:'https://github.com/Andyqwe44/tictactoe'},{l:'Slint',u:'https://slint.dev'},{l:'Tauri 2',u:'https://v2.tauri.app'}].map(x=>
          <button key={x.l} onClick={async()=>{try{const{open}=await import('@tauri-apps/plugin-shell');await open(x.u)}catch{window.open(x.u,'_blank')}}}
            className="block text-sm text-accent hover:underline py-0.5 cursor-pointer">{x.l}</button>
        )}
          <div className="text-xs font-medium text-text-secondary mt-3 mb-1">Credits</div>
          <div className="text-xs text-text-muted">Andyqwe44 · Tauri 2 · React · Tailwind · DXGI · Interception · PyTorch</div>
        </div>
      </div>
      <div className="h-4" />
    </div>
  )
}

export default function App() {
  const [tab, setTab] = useState('Monitor')
  const [running, setRunning] = useState(false)
  const [rightWidth, setRightWidth] = useState(DEFAULT_RIGHT_WIDTH)
  const [rightCollapsed, setRightCollapsed] = useState(false)
  const isResizing = useRef(false)
  const [selWindow, setSelWindow] = useState<WindowInfo>({ title: ' Entire Desktop', category: 'desktop', hwnd: 0 })

  const handleResizeStart = useCallback((e: React.MouseEvent) => {
    e.preventDefault(); isResizing.current = true
    document.body.style.cursor = 'col-resize'; document.body.style.userSelect = 'none'
    const onMove = (ev: MouseEvent) => {
      const w = document.body.clientWidth - ev.clientX
      if (w < 160) setRightCollapsed(true)
      else { setRightCollapsed(false); setRightWidth(Math.max(320, Math.min(800, w))) }
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
          {tab === 'Monitor' && (
            <div className="flex-1 flex items-center justify-center p-6">
              <div className="rounded-xl bg-bg-secondary p-8 text-center space-y-3 max-w-md w-full">
                <div className="text-5xl opacity-20">🎮</div>
                <div className="text-sm text-text-secondary">{running ? 'Task running...' : 'Press Start to begin the agent'}</div>
                <div className="text-xs text-text-muted">Target: {selWindow.title}</div>
              </div>
            </div>
          )}
          {tab === 'Log' && <div className="flex-1 overflow-hidden px-6"><LogPanel /></div>}
          {tab === 'Settings' && <SettingsPage />}
          <BottomBar running={running} fps={0} lat={0} />
        </div>
        <Tooltip text={rightCollapsed ? "向右拖拽展开面板" : "拖拽调整面板宽度，向右拖到底可折叠"}>
          <div onMouseDown={handleResizeStart}
            className={`${rightCollapsed ? 'w-4' : 'w-1'} hover:bg-accent/50 cursor-col-resize flex items-center justify-center group shrink-0 transition-all select-none bg-transparent`}>
            <div className="w-[2px] h-8 rounded-full transition-colors bg-border group-hover:bg-accent" />
          </div>
        </Tooltip>
        {!rightCollapsed && (
          <div className="flex flex-col p-3 gap-3 overflow-y-auto shrink-0" style={{ width: rightWidth, minWidth: 240 }}>
            <ConnectionPanel onSelect={setSelWindow} />
            <ScreenshotPanel selWin={selWindow} />
            <LogPanel />
          </div>
        )}
      </div>
    </div>
  )
}
