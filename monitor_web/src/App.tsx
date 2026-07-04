import { useState, useEffect, useRef, useCallback } from 'react'
import { Play, Square, Camera, Monitor, Settings, Moon, Sun, ChevronUp, ChevronDown, FileText, Trash2, X, MonitorUp, Search, MonitorSmartphone } from 'lucide-react'

// ═══ Tooltip ── 300ms delay, persists while hovering ═══
function Tooltip({ text, children }: { text: string; children: React.ReactElement }) {
  const [show, setShow] = useState(false)
  const timer = useRef<number>(0)
  return (
    <div className="relative inline-flex"
      onMouseEnter={() => { timer.current = window.setTimeout(() => setShow(true), 300) }}
      onMouseLeave={() => { clearTimeout(timer.current); setShow(false) }}
      onMouseMove={() => { if (!show) { clearTimeout(timer.current); timer.current = window.setTimeout(() => setShow(true), 300) } }}>
      {children}
      {show && (
        <div className="absolute bottom-full left-1/2 -translate-x-1/2 mb-1 px-2 py-1 bg-bg-tertiary text-text-primary text-xs rounded shadow-lg whitespace-nowrap z-50 pointer-events-none">
          {text}
        </div>
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
    <button onClick={() => { setDark(!dark); document.documentElement.classList.toggle('dark', !dark) }} title="Toggle theme"
      className="p-2 rounded-md hover:bg-bg-hover transition-colors">
      {dark ? <Sun className="w-4 h-4 text-text-secondary" /> : <Moon className="w-4 h-4 text-text-secondary" />}
    </button>
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
  return (
    <Tooltip text={title}>
      <button onClick={onClick}
        className={`inline-flex items-center gap-1.5 rounded-md px-2.5 h-7 text-xs font-medium transition-all duration-150 ${className ?? ''} ${
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
  tab: string; setTab: (t: string) => void; running: boolean; onStart: () => void; onStop: () => void
}) {
  const tabs = ['Monitor', 'Log', 'Config'] as const
  return (
    <div className="flex items-center h-10 bg-bg-secondary border-b border-border select-none shrink-0">
      <div className="flex-1 flex items-center h-full overflow-x-auto px-1">
        {tabs.map(t => (
          <button title={t} key={t} onClick={() => setTab(t)}
            className={`group flex items-center h-full px-3 cursor-pointer border-r border-border min-w-[90px] transition-colors
              ${t === tab ? 'bg-bg-primary text-accent border-b-2 border-b-accent' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover border-b-2 border-b-transparent'}`}>
            <span className="flex-1 truncate text-sm">{t}</span>
          </button>
        ))}
      </div>
      <div className="flex items-center gap-1 px-2">
        {running
          ? <ActionBtn icon={<Square className="w-3.5 h-3.5" />} label="Stop" title="停止所有运行中的任务" variant="danger" onClick={onStop} />
          : <ActionBtn icon={<Play className="w-3.5 h-3.5" />} label="Start" title="启动agent任务" variant="primary" onClick={onStart} />
        }
        <div className="mx-1 h-4 w-px bg-border" />
        <ThemeBtn />
        <IconBtn title="设置" icon={<Settings className="w-4 h-4" />} />
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
interface WindowInfo { title: string; category: string }

// ═══ Window Picker ───
function WindowPickerModal({ open, onClose, onSelect }: { open: boolean; onClose: () => void; onSelect: (w: WindowInfo) => void }) {
  const [search, setSearch] = useState('')
  const [windows, setWindows] = useState<WindowInfo[]>([])
  const [filter, setFilter] = useState('all')

  useEffect(() => { if (open) { setFilter('all'); loadWindows() } }, [open])
  const loadWindows = async () => {
    try {
      const { invoke } = await import('@tauri-apps/api/core')
      const list = await invoke<WindowInfo[]>('list_windows')
      setWindows(list)
    } catch {
      setWindows([{ title: ' Entire Desktop', category: 'desktop' }, { title: 'Tic Tac Toe — main.exe', category: 'window' }, { title: 'Notepad', category: 'window' }, { title: 'Chrome', category: 'window' }])
    }
  }

  const categories = ['all', 'desktop', 'window'] as const
  const filtered = windows.filter(w => {
    if (filter === 'desktop') return w.category === 'desktop'
    if (filter === 'window') return w.category === 'window'
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
          <button title="关闭" onClick={onClose} className="p-1 rounded-md hover:bg-bg-hover transition-colors"><X className="w-4 h-4 text-text-secondary" /></button>
        </div>
        {/* Category tabs */}
        <div className="flex gap-1 px-4 pt-3 pb-1">
          {categories.map(c => (
            <button title={"筛选: " + c} key={c} onClick={() => setFilter(c)}
              className={`px-3 py-1 rounded-full text-xs font-medium transition-colors capitalize
                ${filter === c ? 'bg-accent text-white' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'}`}>
              {c === 'all' ? 'All' : c === 'desktop' ? ' Desktop' : ' Windows'}
            </button>
          ))}
          <div className="flex-1" />
        </div>
        {/* Search */}
        <div className="px-4 py-2">
          <div className="flex items-center gap-2 h-8 rounded-lg border border-border bg-bg-primary px-3">
            <Search className="w-3.5 h-3.5 text-text-muted shrink-0" />
            <input value={search} onChange={e => setSearch(e.target.value)} placeholder="Search..."
              className="flex-1 bg-transparent text-sm text-text-primary outline-none placeholder:text-text-muted" autoFocus />
          </div>
        </div>
        {/* List */}
        <div className="flex-1 overflow-y-auto px-2 pb-2">
          {filtered.map((w, i) => (
            <button title={"选择: " + w.title} key={i} onClick={() => { onSelect(w); onClose() }}
              className="w-full flex items-center gap-3 px-3 py-2.5 rounded-lg text-left hover:bg-bg-hover transition-colors group">
              {w.category === 'desktop' ? <MonitorSmartphone className="w-4 h-4 text-text-muted group-hover:text-accent shrink-0" />
                : <Monitor className="w-4 h-4 text-text-muted group-hover:text-accent shrink-0" />}
              <div className="flex-1 min-w-0">
                <span className="text-sm text-text-primary truncate block">{w.title}</span>
                <span className="text-xs text-text-muted capitalize">{w.category}</span>
              </div>
            </button>
          ))}
        </div>
        <div className="px-4 py-2 border-t border-border text-xs text-text-muted text-center">
          {filtered.length} items — {windows.filter(w => w.category === 'desktop').length} desktops, {windows.filter(w => w.category === 'window').length} windows
        </div>
      </div>
    </div>
  )
}

// ═══ Connection Panel ───
function ConnectionPanel({ onSelect }: { onSelect: (w: WindowInfo) => void }) {
  const [pickerOpen, setPickerOpen] = useState(false)
  const [title, setTitle] = useState(' Entire Desktop')
  const handleSelect = (w: WindowInfo) => { setTitle(w.title); onSelect(w); addLog(`Selected: ${w.title}`) }
  return (
    <div className="rounded-xl bg-bg-secondary p-4">
      <div className="flex items-center justify-between mb-2">
        <span className="text-xs font-medium text-text-primary">Connection</span>
      </div>
      <div className="space-y-2">
        <div className="flex gap-2">
          <input value={title} onChange={e => setTitle(e.target.value)} placeholder="Window Title"
            className="flex-1 h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm text-text-primary outline-none focus:border-accent transition-colors placeholder:text-text-muted" />
          <button title="选择要截屏的窗口" onClick={() => setPickerOpen(true)}
            className="shrink-0 h-8 px-3 rounded-lg border border-border bg-bg-primary text-sm text-text-secondary hover:bg-bg-hover transition-colors flex items-center gap-1.5">
            <MonitorUp className="w-3.5 h-3.5" /><span>Select</span>
          </button>
        </div>
        <input defaultValue="127.0.0.1:9999" placeholder="Server Address"
          className="w-full h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm text-text-primary outline-none focus:border-accent transition-colors placeholder:text-text-muted" />
      </div>
      <WindowPickerModal open={pickerOpen} onClose={() => setPickerOpen(false)} onSelect={handleSelect} />
    </div>
  )
}

// ═══ Screenshot Panel ───
function ScreenshotPanel() {
  const [expanded, setExpanded] = useState(true)
  const [previewing, setPreviewing] = useState(false)
  const [imgSrc, setImgSrc] = useState('')
  const [fps, setFps] = useState(0)
  const timerRef = useRef<number>(0)

  const togglePreview = () => {
    if (previewing) {
      setPreviewing(false); setFps(0); setImgSrc('')
      if (timerRef.current) clearInterval(timerRef.current)
      addLog('Preview stopped')
    } else {
      setPreviewing(true)
      addLog('Starting preview @ 20fps...')
      let frames = 0; let last = Date.now()
      const capture = async () => {
        try {
          const { invoke } = await import('@tauri-apps/api/core')
          const b64 = await invoke<string>('capture_screenshot', { monitorIdx: 0 })
          setImgSrc(`data:image/png;base64,${b64}`)
          frames++
          const now = Date.now(); const elapsed = now - last
          if (elapsed >= 1000) { setFps(Math.round(frames * 1000 / elapsed)); frames = 0; last = now }
        } catch { /* browser fallback */ }
      }
      capture()
      timerRef.current = window.setInterval(capture, 50)
    }
  }

  return (
    <div className="mt-3 rounded-xl bg-bg-secondary p-4">
      <div className="flex items-center justify-between mb-2">
        <span className="text-xs font-medium text-text-primary">Screenshot</span>
        <div className="flex items-center gap-1">
          <IconBtn title="单帧截图" icon={<Camera className="w-3.5 h-3.5" />}
            onClick={async () => {
              addLog('Capturing screenshot...')
              try { const { invoke } = await import('@tauri-apps/api/core')
                const b64 = await invoke<string>('capture_single')
                setImgSrc(`data:image/png;base64,${b64}`)
                addLog('Screenshot captured')
              } catch { addLog('Screenshot failed (browser mode)') }
            }} />
          {previewing
            ? <ActionBtn icon={<Square className="w-3 h-3" />} label="Stop" title="停止截屏预览" variant="danger" onClick={togglePreview} />
            : <ActionBtn icon={<Play className="w-3 h-3" />} label="Preview" title="开始实时截屏预览 (20 FPS)" variant="primary" onClick={togglePreview} />
          }
          <IconBtn title={expanded ? "折叠截图面板" : "展开截图面板"}
            icon={expanded ? <ChevronUp className="w-3.5 h-3.5" /> : <ChevronDown className="w-3.5 h-3.5" />}
            onClick={() => setExpanded(!expanded)} />
        </div>
      </div>
      {expanded && (
        <div className="min-h-[160px] rounded-lg bg-bg-primary overflow-hidden flex items-center justify-center">
          {imgSrc ? (
            <img src={imgSrc} className="w-full h-auto object-contain" alt="preview" />
          ) : (
            <span className="text-sm text-text-muted">{previewing ? 'Capturing...' : 'Press Preview'}</span>
          )}
          {previewing && <div className="absolute bottom-2 right-2 text-xs text-accent bg-bg-primary/80 px-2 py-0.5 rounded">{fps} FPS</div>}
        </div>
      )}
    </div>
  )
}

// ═══ Log Panel ───
function LogPanel() {
  const [expanded, setExpanded] = useState(true)
  const [logs, setLogs] = useState(gLogs)
  useEffect(() => { const fn = () => setLogs([...gLogs]); gLogListeners.push(fn); return () => { gLogListeners = gLogListeners.filter(f => f !== fn) } }, [])
  return (
    <div className="mt-3 rounded-xl bg-bg-secondary p-4">
      <div className="flex items-center justify-between mb-2">
        <span className="text-xs font-medium text-text-primary">Log</span>
        <div className="flex items-center gap-1">
          <IconBtn title="运行日志" icon={<FileText className="w-3.5 h-3.5" />} />
          <IconBtn title="清空日志" icon={<Trash2 className="w-3.5 h-3.5" />} onClick={() => { gLogs = []; gLogListeners.forEach(f => f()) }} />
          <IconBtn icon={expanded ? <ChevronUp className="w-3.5 h-3.5" /> : <ChevronDown className="w-3.5 h-3.5" />} title={expanded ? "折叠" : "展开"} onClick={() => setExpanded(!expanded)} />
        </div>
      </div>
      {expanded && (
        <div className="min-h-[200px] max-h-[300px] overflow-y-auto rounded-lg bg-bg-primary p-3">
          {logs.length === 0 ? (
            <div className="flex items-center justify-center h-full min-h-[180px] text-sm text-text-muted">No logs</div>
          ) : (
            <div className="space-y-1 font-mono text-xs text-text-secondary">
              {logs.map((l, i) => (
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
export default function App() {
  const [tab, setTab] = useState('Monitor')
  const [running, setRunning] = useState(false)
  const [rightWidth, setRightWidth] = useState(DEFAULT_RIGHT_WIDTH)
  const [rightCollapsed, setRightCollapsed] = useState(false)
  const isResizing = useRef(false)
  const [selWindow, setSelWindow] = useState<WindowInfo>({ title: ' Entire Desktop', category: 'desktop' })

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
          <div className="flex-1 flex items-center justify-center p-4">
            <div className="text-center space-y-3">
              <div className="text-5xl opacity-20">🎮</div>
              <div className="text-sm text-text-secondary">{running ? 'Task running...' : 'Press Start to begin the agent'}</div>
              <div className="text-xs text-text-muted">Target: {selWindow.title}</div>
            </div>
          </div>
          <BottomBar running={running} fps={0} lat={0} />
        </div>
        <div onMouseDown={handleResizeStart}
          className={`${rightCollapsed ? 'w-4' : 'w-1'} hover:bg-accent/50 cursor-col-resize flex items-center justify-center group shrink-0 transition-all select-none bg-transparent`}>
          <div className="w-[2px] h-8 rounded-full transition-colors bg-border group-hover:bg-accent" />
        </div>
        {!rightCollapsed && (
          <div className="flex flex-col p-3 gap-3 overflow-y-auto shrink-0" style={{ width: rightWidth, minWidth: 240 }}>
            <ConnectionPanel onSelect={setSelWindow} />
            <ScreenshotPanel />
            <LogPanel />
          </div>
        )}
      </div>
    </div>
  )
}
