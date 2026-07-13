// ═══ Settings View ───
import { useState, useEffect, useRef, useCallback } from 'react'
import {
  Camera, Play, Cpu, Sun, RefreshCw, ChevronDown,
  Monitor, MousePointer2, Keyboard, Pencil, FolderOpen,
} from 'lucide-react'
import { Tooltip, ActionBtn } from './Toolkit'
import { ConnectionPanel } from './ConnectionPanel'
import { hostCall, addLog } from '../lib/bridge'
import {
  COLLAPSIBLE_HEADER, SELECTABLE_BTN, CAPTURE_METHODS, RENDER_METHODS,
  MOUSE_MODES, KEYBOARD_MODES, codeToName,
} from '../lib/constants'
import type { WindowInfo } from '../lib/types'

// ── Darken hex color by percentage (0–100) for hover state ──
function darken(hex: string, pct: number): string {
  const v = parseInt(hex.slice(1), 16)
  const r = Math.max(0, ((v >> 16) & 0xff) - Math.round(255 * pct / 100))
  const g = Math.max(0, ((v >> 8) & 0xff) - Math.round(255 * pct / 100))
  const b = Math.max(0, (v & 0xff) - Math.round(255 * pct / 100))
  return `#${((r << 16) | (g << 8) | b).toString(16).padStart(6, '0')}`
}

// ── SettingsCard (collapsible) ──
export function SettingsCard({
  icon,
  title,
  defaultExpanded,
  children,
}: {
  icon: React.ReactNode
  title: string
  defaultExpanded?: boolean
  children: React.ReactNode
}) {
  const [expanded, setExpanded] = useState(defaultExpanded ?? true)
  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
      <div
        role="button"
        tabIndex={0}
        onClick={() => {
          setExpanded(!expanded)
          addLog(`[Settings] ${title} ${!expanded ? 'expanded' : 'collapsed'}`)
        }}
        onKeyDown={(e) => {
          if (e.key === 'Enter' || e.key === ' ') {
            ;(e.currentTarget as HTMLElement).click()
          }
        }}
        className={COLLAPSIBLE_HEADER}
      >
        <div className="flex items-center gap-2">
          {icon}
          <span className="text-sm font-medium text-text-primary">{title}</span>
        </div>
        <ChevronDown
          className={`w-4 h-4 text-text-muted transition-transform duration-150 ${expanded ? 'rotate-180' : ''}`}
        />
      </div>
      <div
        className="grid transition-[grid-template-rows] duration-150 ease-out"
        style={{ gridTemplateRows: expanded ? '1fr' : '0fr' }}
      >
        <div className="overflow-hidden min-h-0">
          <div className="border-t border-border" />
          <div className="p-4">{children}</div>
        </div>
      </div>
    </div>
  )
}

// ── Status Bar ──
function StatusBar({ screen, appVersion }: { screen: string; appVersion: string }) {
  return (
    <div className="flex items-center gap-4 px-4 py-2.5 bg-bg-secondary rounded-xl ring-1 ring-inset ring-border text-xs text-text-secondary">
      <span className="flex items-center gap-1.5">
        <Monitor className="w-3.5 h-3.5" />
        {screen}
      </span>
      <span className="text-border">|</span>
      <span className="flex items-center gap-1.5">
        <RefreshCw className="w-3.5 h-3.5" />
        {appVersion}
      </span>
      <span className="text-border">|</span>
      <span className="flex items-center gap-1.5">
        <span className="w-2 h-2 rounded-full bg-success" />
        Ready
      </span>
      <span className="flex-1" />
      <span className="text-text-muted hidden sm:inline">Game Agent Monitor</span>
    </div>
  )
}

// ═══ SettingsView ═══
export function SettingsView({
  snapMethod, setSnapMethod, streamMethod, setStreamMethod,
  renderMethod, setRenderMethod,
  autoSnap, setAutoSnap, autoStream, setAutoStream,
  selWin, winState, expectedCaptureState, setExpectedCaptureState,
  onSelect, onDisconnect,
  keepFiles, setKeepFiles, appVersion,
  theme, setTheme,
  devMode, setDevMode,
  mouseMode, setMouseMode,
  keyMode, setKeyMode,
  mappingHotkey, setMappingHotkey,
  selfTargetMode, setSelfTargetMode,
  onCheckUpdate,
  hasUpdate,
  isAdmin,
  onSwitchPermission,
}: {
  snapMethod: string; setSnapMethod: (m: string) => void
  streamMethod: string; setStreamMethod: (m: string) => void
  renderMethod: string; setRenderMethod: (m: string) => void
  autoSnap?: boolean; setAutoSnap?: (v: boolean) => void
  autoStream?: boolean; setAutoStream?: (v: boolean) => void
  selWin?: WindowInfo; winState: string
  expectedCaptureState?: string; setExpectedCaptureState?: (s: string) => void
  onSelect: (w: WindowInfo) => void; onDisconnect: () => void
  keepFiles: number; setKeepFiles: (n: number) => void
  appVersion: string
  theme: string; setTheme: (t: 'light' | 'dark' | 'system') => void
  devMode: boolean; setDevMode: (v: boolean) => void
  mouseMode: 'seize' | 'semi' | 'background'; setMouseMode: (m: 'seize' | 'semi' | 'background') => void
  keyMode: 'seize' | 'postmsg' | 'sendmsg'; setKeyMode: (m: 'seize' | 'postmsg' | 'sendmsg') => void
  mappingHotkey: string; setMappingHotkey: (k: string) => void
  selfTargetMode: 'warn' | 'exclude'; setSelfTargetMode: (m: 'warn' | 'exclude') => void
  onCheckUpdate?: () => void
  hasUpdate?: boolean
  isAdmin?: boolean
  onSwitchPermission?: (toAdmin: boolean) => void
}) {
  const themePairs = [
    ['#3B82F6', '#F97316'], // Ocean — blue + orange
    ['#6366F1', '#EAB308'], // Twilight — indigo + yellow
    ['#10B981', '#F43F5E'], // Lagoon — emerald + rose
    ['#F59E0B', '#06B6D4'], // Sunset — amber + cyan
    ['#EC4899', '#6366F1'], // Orchid — pink + indigo
    ['#14B8A6', '#F97316'], // Mint — teal + orange
    ['#3B82F6', '#8B5CF6'], // Nebula — blue + violet
    ['#EF4444', '#22C55E'], // Dev ⚡ — red danger + hacker green
  ]
  const themeNames = ['Ocean', 'Twilight', 'Lagoon', 'Sunset', 'Orchid', 'Mint', 'Nebula', 'Dev']
  const [accent, setAccent] = useState(() => {
    const v = document.documentElement.style.getPropertyValue('--color-accent').trim()
    return v || '#3B82F6'
  })
  const [secondaryAccent, setSecondaryAccent] = useState(() => {
    const v = document.documentElement.style.getPropertyValue('--color-accent-secondary').trim()
    return v || '#F97316'
  })
  const DEV_PAIR = themePairs[7] // ['#EF4444', '#22C55E']
  const [normalAccent, setNormalAccent] = useState(accent)
  const [normalSecondaryAccent, setNormalSecondaryAccent] = useState(secondaryAccent)
  const [devAccent, setDevAccent] = useState(DEV_PAIR[0])
  const [devSecondaryAccent, setDevSecondaryAccent] = useState(DEV_PAIR[1])

  // Sync accent-hover on mount (CSS default may be stale)
  useEffect(() => {
    document.documentElement.style.setProperty('--color-accent-hover', darken(accent, 15))
  }, []) // eslint-disable-line react-hooks/exhaustive-deps
  const [screenRes, setScreenRes] = useState('?×?')
  const [logDir, setLogDir] = useState('...')
  const [connExpanded, setConnExpanded] = useState(true)
  // ── Key recording (sequence-based: press order matters) ──
  const [recording, setRecording] = useState(false)
  const [displayCombo, setDisplayCombo] = useState('')
  const pressedSeqRef = useRef<string[]>([])             // ordered e.code values in press order
  const lastComboRef = useRef('')                         // cached combo string (survives onUp splice)
  const savedComboRef = useRef(mappingHotkey)             // pre-recording value for cancel

  // ── Modifier-only warning ──
  // Check if current hotkey consists solely of modifier keys (Ctrl/Alt/Shift/Win).
  // Pure-modifier combos are technically valid but prone to OS-level conflicts.
  const MODIFIER_NAMES = new Set(['Ctrl', 'Alt', 'Shift', 'Win'])
  const isModifierOnly = mappingHotkey.split('+').every((k) => MODIFIER_NAMES.has(k))

  const startRecording = useCallback(() => {
    savedComboRef.current = mappingHotkey
    lastComboRef.current = ''
    pressedSeqRef.current = []
    setDisplayCombo('')
    setRecording(true)
  }, [mappingHotkey])

  // Stable — uses refs, no state deps (safe in useEffect)
  const commitRecording = useCallback(() => {
    setRecording(false)
    const combo = lastComboRef.current
    if (combo) {
      setMappingHotkey(combo)
      savedComboRef.current = combo
      addLog(`[Setting] mapping hotkey = ${combo}`)
    } else {
      setDisplayCombo(savedComboRef.current)
    }
    pressedSeqRef.current = []
  }, [setMappingHotkey])

  const cancelRecording = useCallback(() => {
    setRecording(false)
    setDisplayCombo(savedComboRef.current)
    pressedSeqRef.current = []
  }, [])

  // Keyboard listeners during recording
  useEffect(() => {
    if (!recording) return
    const onDown = (e: KeyboardEvent) => {
      e.preventDefault()
      e.stopPropagation()
      if (e.repeat) return // ignore auto-repeat
      // Add code if not already present (handles L/R Ctrl both → different codes)
      if (!pressedSeqRef.current.includes(e.code)) {
        pressedSeqRef.current.push(e.code)
      }
      const combo = pressedSeqRef.current.map(codeToName).join('+')
      lastComboRef.current = combo
      setDisplayCombo(combo)
    }
    const onUp = (e: KeyboardEvent) => {
      const idx = pressedSeqRef.current.indexOf(e.code)
      if (idx >= 0) pressedSeqRef.current.splice(idx, 1)
      requestAnimationFrame(() => {
        if (pressedSeqRef.current.length === 0) {
          commitRecording()
        }
      })
    }
    window.addEventListener('keydown', onDown, true)
    window.addEventListener('keyup', onUp, true)
    return () => {
      window.removeEventListener('keydown', onDown, true)
      window.removeEventListener('keyup', onUp, true)
    }
  }, [recording, commitRecording])

  // ── Hotkey test indicator: flash when the configured combo is pressed ──
  const [triggerFlash, setTriggerFlash] = useState(false)
  const testSeqRef = useRef<string[]>([])
  useEffect(() => {
    if (recording) return // don't test while recording
    const onDown = (e: KeyboardEvent) => {
      if (e.repeat) return
      if (!testSeqRef.current.includes(e.code)) {
        testSeqRef.current.push(e.code)
      }
      if (testSeqRef.current.map(codeToName).join('+') === mappingHotkey) {
        setTriggerFlash(true)
        setTimeout(() => setTriggerFlash(false), 400)
      }
    }
    const onUp = (e: KeyboardEvent) => {
      const idx = testSeqRef.current.indexOf(e.code)
      if (idx >= 0) testSeqRef.current.splice(idx, 1)
    }
    window.addEventListener('keydown', onDown, true)
    window.addEventListener('keyup', onUp, true)
    return () => {
      window.removeEventListener('keydown', onDown, true)
      window.removeEventListener('keyup', onUp, true)
    }
  }, [mappingHotkey, recording])

  useEffect(() => {
    hostCall('screen_info')
      .then((si: any) => setScreenRes(`${si.w}×${si.h}`))
      .catch(() => {})
  }, [])
  useEffect(() => {
    hostCall('get_log_dir')
      .then((res: any) => {
        if (res?.dir) setLogDir(res.dir)
      })
      .catch(() => {})
  }, [])

  // Dev mode: auto-switch to Dev accent (remembered), restore normal on exit
  useEffect(() => {
    if (devMode) {
      setNormalAccent(accent)
      setNormalSecondaryAccent(secondaryAccent)
      setAccent(devAccent)
      setSecondaryAccent(devSecondaryAccent)
      document.documentElement.style.setProperty('--color-accent', devAccent)
      document.documentElement.style.setProperty('--color-accent-secondary', devSecondaryAccent)
      document.documentElement.style.setProperty('--color-accent-hover', darken(devAccent, 15))
    } else {
      setDevAccent(accent)
      setDevSecondaryAccent(secondaryAccent)
      setAccent(normalAccent)
      setSecondaryAccent(normalSecondaryAccent)
      document.documentElement.style.setProperty('--color-accent', normalAccent)
      document.documentElement.style.setProperty('--color-accent-secondary', normalSecondaryAccent)
      document.documentElement.style.setProperty('--color-accent-hover', darken(normalAccent, 15))
    }
  }, [devMode])

  return (
    <div className="flex-1 overflow-y-auto p-6 space-y-3">
      <StatusBar screen={screenRes} appVersion={appVersion} />

      <ConnectionPanel
        onSelect={onSelect} onDisconnect={onDisconnect}
        snapMethod={snapMethod} setSnapMethod={setSnapMethod}
        streamMethod={streamMethod} setStreamMethod={setStreamMethod}
        selWin={selWin} winState={winState}
        expectedCaptureState={expectedCaptureState}
        setExpectedCaptureState={setExpectedCaptureState}
        expanded={connExpanded} onToggle={() => setConnExpanded((v) => !v)}
      />

      <SettingsCard
        icon={<Camera className="w-4 h-4 text-text-secondary" />}
        title="Capture"
      >
        <div className="space-y-3">
          {/* Snapshot + Stream side by side */}
          <div className="flex gap-0">
            <div className="flex-1 space-y-2 pr-3">
              <div className="flex items-center justify-between">
                <span className="text-xs text-text-muted inline-flex items-center gap-1">
                  <Camera className="w-3.5 h-3.5" /> Snapshot
                </span>
                <label className="flex items-center gap-1.5 cursor-pointer select-none">
                  <span className="text-xs text-text-muted">Auto</span>
                  <Tooltip text="自动根据窗口状态选择截图方式：前台/后台→WGC，桌面/最小化→DXGI">
                  <button
                    onClick={(e) => {
                      e.stopPropagation()
                      const next = !autoSnap
                      setAutoSnap?.(next)
                      if (next && selWin) {
                        const isDesktop = selWin.hwnd === 0
                        setSnapMethod(
                          isDesktop || winState === 'minimized' ? 'dxgi' : 'wgc',
                        )
                      }
                      addLog(`[Setting] auto snap = ${next}`)
                    }}
                    className={`relative w-10 h-5 rounded-full transition-colors ${autoSnap ? 'bg-accent-secondary' : 'bg-bg-tertiary'}`}
                  >
                    <span
                      className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${autoSnap ? 'translate-x-5' : ''}`}
                    />
                  </button>
                  </Tooltip>
                </label>
              </div>
              <div className="flex flex-col gap-2">
                {CAPTURE_METHODS.map((m) => {
                  const isActive = snapMethod === m.v
                  const ringClass = !autoSnap && isActive
                    ? 'border-accent bg-accent/10 cursor-pointer'
                    : autoSnap && isActive
                      ? 'border-accent-secondary bg-accent-secondary/10'
                      : autoSnap
                        ? 'border-border bg-bg-primary opacity-50 cursor-not-allowed'
                        : 'border-border bg-bg-primary hover:bg-bg-hover cursor-pointer'
                  const tagClass = autoSnap && isActive
                    ? 'text-accent-secondary bg-accent-secondary/10'
                    : 'text-accent bg-accent/10'
                  return (
                    <Tooltip key={m.v} text={m.desc}>
                      <label className={`${SELECTABLE_BTN} ${ringClass}`}>
                        <input
                          type="radio" name="snapMethod" value={m.v}
                          checked={isActive} disabled={autoSnap}
                          onChange={(e) => {
                            if (!autoSnap) {
                              setSnapMethod(e.target.value)
                              addLog(`[Setting] snap method = ${e.target.value}`)
                            }
                          }}
                          className="sr-only"
                        />
                        <span className="text-xs font-medium text-text-primary">
                          {m.name} <span className="text-text-muted">({m.eng})</span>
                        </span>
                        <span className="ml-auto flex items-center gap-1">
                          {m.rec.split('/').map((t: string) => (
                            <span
                              key={t}
                              className={`text-[11px] font-medium px-1.5 py-0.5 rounded ${tagClass}`}
                            >
                              {t}
                            </span>
                          ))}
                        </span>
                      </label>
                    </Tooltip>
                  )
                })}
              </div>
            </div>
            <div className="w-px bg-border shrink-0" />
            <div className="flex-1 space-y-2 pl-3">
              <div className="flex items-center justify-between">
                <span className="text-xs text-text-muted inline-flex items-center gap-1">
                  <Play className="w-3.5 h-3.5" /> Stream
                </span>
                <label className="flex items-center gap-1.5 cursor-pointer select-none">
                  <span className="text-xs text-text-muted">Auto</span>
                  <Tooltip text="自动选择流传输方式（当前仅 WGC 支持实时流）">
                  <button
                    onClick={(e) => {
                      e.stopPropagation()
                      const next = !autoStream
                      setAutoStream?.(next)
                      if (next) setStreamMethod('wgc')
                      addLog(`[Setting] auto stream = ${next}`)
                    }}
                    className={`relative w-10 h-5 rounded-full transition-colors ${autoStream ? 'bg-accent-secondary' : 'bg-bg-tertiary'}`}
                  >
                    <span
                      className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${autoStream ? 'translate-x-5' : ''}`}
                    />
                  </button>
                  </Tooltip>
                </label>
              </div>
              <div className="flex flex-col gap-2">
                {CAPTURE_METHODS.map((m) => {
                  const unsupported = m.v === 'dxgi'
                  const isActive = streamMethod === m.v
                  const locked = autoStream || unsupported
                  const ringClass =
                    !autoStream && isActive && !unsupported
                      ? 'border-accent bg-accent/10 cursor-pointer'
                      : autoStream && isActive
                        ? 'border-accent-secondary bg-accent-secondary/10'
                        : locked
                          ? 'border-border bg-bg-primary opacity-50 cursor-not-allowed'
                          : 'border-border bg-bg-primary hover:bg-bg-hover cursor-pointer'
                  const tagClass =
                    autoStream && isActive
                      ? 'text-accent-secondary bg-accent-secondary/10'
                      : 'text-accent bg-accent/10'
                  return (
                    <Tooltip
                      key={m.v}
                      text={
                        unsupported
                          ? 'DXGI 流未实现，仅 WGC 支持实时预览'
                          : m.desc
                      }
                    >
                      <label className={`${SELECTABLE_BTN} ${ringClass}`}>
                        <input
                          type="radio" name="streamMethod" value={m.v}
                          checked={isActive}
                          disabled={locked}
                          onChange={(e) => {
                            if (!locked) {
                              setStreamMethod(e.target.value)
                              addLog(`[Setting] stream method = ${e.target.value}`)
                            }
                          }}
                          className="sr-only"
                        />
                        <span className="text-xs font-medium text-text-primary">
                          {m.name} <span className="text-text-muted">({m.eng})</span>
                        </span>
                        <span className="ml-auto flex items-center gap-1">
                          {unsupported ? (
                            <span className="text-[11px] font-medium text-text-muted bg-bg-tertiary px-1.5 py-0.5 rounded">
                              未实现
                            </span>
                          ) : (
                            m.rec.split('/').map((t: string) => (
                              <span
                                key={t}
                                className={`text-[11px] font-medium px-1.5 py-0.5 rounded ${tagClass}`}
                              >
                                {t}
                              </span>
                            ))
                          )}
                        </span>
                      </label>
                    </Tooltip>
                  )
                })}
              </div>
            </div>
          </div>
          {/* Render Method */}
          <div className="border-t border-border pt-3 space-y-2">
            <div className="flex items-center justify-between">
              <span className="text-xs text-text-muted inline-flex items-center gap-1">
                <Monitor className="w-3.5 h-3.5" /> Render Method
              </span>
            </div>
            <div className="flex flex-col gap-2">
              {RENDER_METHODS.map((m) => {
                const isActive = renderMethod === m.v
                const implemented = m.v === 'shared'
                const ringClass =
                  isActive && implemented
                    ? 'border-accent bg-accent/10 cursor-pointer'
                    : implemented
                      ? 'border-border bg-bg-primary hover:bg-bg-hover cursor-pointer'
                      : 'border-border bg-bg-primary opacity-50 cursor-not-allowed'
                return (
                  <Tooltip key={m.v} text={m.desc}>
                    <label className={`${SELECTABLE_BTN} ${ringClass}`}>
                      <input
                        type="radio" name="renderMethod" value={m.v}
                        checked={isActive} disabled={!implemented}
                        onChange={(e) => {
                          if (implemented) {
                            setRenderMethod(e.target.value)
                            addLog(`[Setting] render method = ${e.target.value}`)
                          }
                        }}
                        className="sr-only"
                      />
                      <span className="text-xs font-medium text-text-primary">
                        {m.name} <span className="text-text-muted">({m.eng})</span>
                      </span>
                      <span className="ml-auto flex items-center gap-1">
                        {m.rec.split('/').map((t: string) => (
                          <span
                            key={t}
                            className={`text-[11px] font-medium px-1.5 py-0.5 rounded ${implemented ? 'text-accent bg-accent/10' : 'text-text-muted bg-bg-tertiary'}`}
                          >
                            {t}
                          </span>
                        ))}
                      </span>
                    </label>
                  </Tooltip>
                )
              })}
            </div>
          </div>
          {/* ── Mouse Mode ── */}
          <div className="border-t border-border pt-3 space-y-2">
            <div className="flex items-center justify-between">
              <span className="text-xs text-text-muted inline-flex items-center gap-1">
                <MousePointer2 className="w-3.5 h-3.5" /> Mouse Mode
              </span>
              <span className="text-[10px] text-text-muted">虚拟指示器常驻 — 不影响本地鼠标使用</span>
            </div>
            <div className="flex flex-col gap-2">
              {MOUSE_MODES.map((m) => {
                const isActive = mouseMode === m.v
                return (
                  <Tooltip key={m.v} text={m.desc}>
                    <label className={`${SELECTABLE_BTN} ${
                      isActive
                        ? 'border-accent bg-accent/10 cursor-pointer'
                        : 'border-border bg-bg-primary hover:bg-bg-hover cursor-pointer'
                    }`}>
                      <input
                        type="radio" name="mouseMode" value={m.v}
                        checked={isActive}
                        onChange={() => { setMouseMode(m.v); addLog(`[Setting] mouse mode = ${m.v}`) }}
                        className="sr-only"
                      />
                      <span className="text-xs font-medium text-text-primary">
                        {m.name} <span className="text-text-muted">({m.eng})</span>
                      </span>
                      <span className="ml-auto">
                        <span className={`text-[11px] font-medium px-1.5 py-0.5 rounded ${
                          isActive ? 'text-accent bg-accent/10' : 'text-text-muted bg-bg-tertiary'
                        }`}>
                          {m.rec}
                        </span>
                      </span>
                    </label>
                  </Tooltip>
                )
              })}
            </div>
          </div>
          {/* ── Keyboard Mode ── */}
          <div className="border-t border-border pt-3 space-y-2">
            <div className="flex items-center justify-between">
              <span className="text-xs text-text-muted inline-flex items-center gap-1">
                <Keyboard className="w-3.5 h-3.5" /> Keyboard Mode
              </span>
              <span className="text-[10px] text-text-muted">点击预览画面获取焦点后，键盘输入转发到目标窗口</span>
            </div>
            <div className="flex flex-col gap-2">
              {KEYBOARD_MODES.map((m) => {
                const isActive = keyMode === m.v
                return (
                  <Tooltip key={m.v} text={m.desc}>
                    <label className={`${SELECTABLE_BTN} ${
                      isActive
                        ? 'border-accent bg-accent/10 cursor-pointer'
                        : 'border-border bg-bg-primary hover:bg-bg-hover cursor-pointer'
                    }`}>
                      <input
                        type="radio" name="keyMode" value={m.v}
                        checked={isActive}
                        onChange={() => { setKeyMode(m.v); addLog(`[Setting] key mode = ${m.v}`) }}
                        className="sr-only"
                      />
                      <span className="text-xs font-medium text-text-primary">
                        {m.name} <span className="text-text-muted">({m.eng})</span>
                      </span>
                      <span className="ml-auto">
                        <span className={`text-[11px] font-medium px-1.5 py-0.5 rounded ${
                          isActive ? 'text-accent bg-accent/10' : 'text-text-muted bg-bg-tertiary'
                        }`}>
                          {m.rec}
                        </span>
                      </span>
                    </label>
                  </Tooltip>
                )
              })}
            </div>
            <div className="text-[11px] text-text-muted space-y-1">
              <div>• 普通按键 — <code className="text-accent bg-accent/10 px-1 rounded">keydown</code> / <code className="text-accent bg-accent/10 px-1 rounded">keyup</code></div>
              <div>• 组合键 — 自动识别（Ctrl+C 等，Ctrl 先按下再按 C）</div>
              <div>• <code className="text-accent bg-accent/10 px-1 rounded">Esc</code> 或外部点击 → 释放焦点，自动松开所有已按下按键</div>
            </div>
          </div>
          {/* ── Self-target avoidance mode ── */}
          <div className="border-t border-border pt-3 space-y-2">
            <div className="flex items-center justify-between">
              <span className="text-xs text-text-muted inline-flex items-center gap-1">
                <MousePointer2 className="w-3.5 h-3.5" /> 自指规避
              </span>
              <span className="text-[10px] text-text-muted">
                桌面捕获时，映射坐标可能落在 GAM 自身窗口
              </span>
            </div>
            <div className="flex flex-col gap-2">
              {[
                {
                  v: 'warn' as const,
                  name: '红色警告',
                  eng: 'Visual Warning',
                  desc: '光标移至 GAM 窗口区域时变红并弹出警告 — 简单安全',
                },
                {
                  v: 'exclude' as const,
                  name: '排除窗口',
                  eng: 'Exclude from Capture',
                  desc: '桌面捕获画面中隐藏 GAM 窗口 — 需要 Windows 10 2004+',
                },
              ].map((m) => {
                const isActive = selfTargetMode === m.v
                return (
                  <Tooltip key={m.v} text={m.desc}>
                    <label
                      className={`${SELECTABLE_BTN} ${
                        isActive
                          ? 'border-accent bg-accent/10 cursor-pointer'
                          : 'border-border bg-bg-primary hover:bg-bg-hover cursor-pointer'
                      }`}
                    >
                      <input
                        type="radio"
                        name="selfTargetMode"
                        value={m.v}
                        checked={isActive}
                        onChange={() => setSelfTargetMode(m.v)}
                        className="sr-only"
                      />
                      <span className="text-xs font-medium text-text-primary">
                        {m.name} <span className="text-text-muted">({m.eng})</span>
                      </span>
                      <span className="ml-auto">
                        <span
                          className={`text-[11px] font-medium px-1.5 py-0.5 rounded ${
                            isActive ? 'text-accent bg-accent/10' : 'text-text-muted bg-bg-tertiary'
                          }`}
                        >
                          {m.v === 'warn' ? '默认' : 'Win10+'}
                        </span>
                      </span>
                    </label>
                  </Tooltip>
                )
              })}
            </div>
          </div>
        </div>
      </SettingsCard>

      <SettingsCard icon={<Cpu className="w-4 h-4 text-text-secondary" />} title="Model">
        <div className="text-xs text-text-muted mb-2">
          Base model + fine-tuning adapter for game-specific AI.
        </div>
        <div className="flex items-center gap-3 mb-2">
          <label className="text-sm text-text-secondary w-24 shrink-0">Model</label>
          <Tooltip text="基础视觉模型" className="flex-1 min-w-0">
            <input
              defaultValue="GenericAgent v1"
              onBlur={(e) => addLog(`[Setting] base model = ${e.target.value}`)}
              className="w-full h-7 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent"
            />
          </Tooltip>
        </div>
        <div className="flex items-center gap-3">
          <label className="text-sm text-text-secondary w-24 shrink-0">Adapter</label>
          <Tooltip text="游戏微调权重" className="flex-1 min-w-0">
            <input
              defaultValue="tictactoe-finetune"
              onBlur={(e) => addLog(`[Setting] adapter = ${e.target.value}`)}
              className="w-full h-7 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent"
            />
          </Tooltip>
        </div>
      </SettingsCard>

      <SettingsCard icon={<Sun className="w-4 h-4 text-text-secondary" />} title="General">
        <div className="space-y-3">
          {/* 运行权限（普通/管理员）— 切换 = 重启到目标权限 */}
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">运行权限</label>
            <div className="flex gap-1 items-center">
              {([['普通', false], ['管理员', true]] as [string, boolean][]).map(([l, v]) => (
                <Tooltip key={String(v)} text={v ? '管理员权限 — 可操作以管理员运行的目标；点击弹 UAC 并重启程序' : '普通用户权限（默认）— 操作普通程序足够，无 UAC 打扰'}>
                  <button
                    onClick={() => onSwitchPermission?.(v)}
                    className={`px-3 py-1 rounded-full text-xs font-medium transition-colors ${!!isAdmin === v ? 'bg-accent text-white' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'}`}
                  >
                    {l}
                  </button>
                </Tooltip>
              ))}
              <span className={`ml-1 text-[10px] px-1.5 py-0.5 rounded ${isAdmin ? 'text-accent bg-accent/10' : 'text-text-muted bg-bg-tertiary'}`}>
                当前: {isAdmin ? '管理员' : '普通'}
              </span>
            </div>
          </div>
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">Theme</label>
            <div className="flex gap-1">
              {[
                ['Light', 'light', '亮色主题 — 白底黑字'],
                ['Dark', 'dark', '暗色主题 — VSCode 风格深蓝灰'],
                ['System', 'system', '跟随系统 — 自动切换亮暗'],
              ].map(([l, v, tip]) => (
                <Tooltip key={v} text={tip}>
                <button
                  onClick={() => {
                    setTheme(v as 'light' | 'dark' | 'system')
                    addLog(`[Theme] ${l}`)
                  }}
                  className={`px-3 py-1 rounded-full text-xs font-medium transition-colors ${theme === v ? 'bg-accent text-white' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'}`}
                >
                  {l}
                </button>
                </Tooltip>
              ))}
            </div>
          </div>
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">Accent</label>
            <div className="flex gap-1.5">
              {themePairs.map(([c1, c2], i) => {
                const isDev = i === themePairs.length - 1
                const disabled = isDev ? !devMode : devMode
                // Selected: compare against the SAVED color for this button's mode,
                // NOT the current accent. Disabled buttons still show their indicator.
                const selected = isDev
                  ? (devAccent === c1 && devSecondaryAccent === c2)
                  : (normalAccent === c1 && normalSecondaryAccent === c2)
                const name = themeNames[i]
                return (
                  <Tooltip key={`${c1}-${c2}`} text={name} className={isDev ? 'ml-3' : ''}>
                  <span className="relative inline-flex items-center justify-center" style={{ width: 28, height: 28 }}>
                    {/* Selected indicator: top line (c1) + bottom line (c2). Dim when disabled. */}
                    {selected && (
                      <>
                        <span
                          className="absolute rounded-full"
                          style={{ width: 20, height: 2, top: 0, left: '50%', transform: 'translateX(-50%)', background: c1, opacity: disabled ? 0.3 : 1 }}
                        />
                        <span
                          className="absolute rounded-full"
                          style={{ width: 20, height: 2, bottom: 0, left: '50%', transform: 'translateX(-50%)', background: c2, opacity: disabled ? 0.3 : 1 }}
                        />
                      </>
                    )}
                    {/* Button (20×20) */}
                    <button
                      onClick={() => {
                        if (disabled) return
                        setAccent(c1)
                        setSecondaryAccent(c2)
                        if (isDev) {
                          setDevAccent(c1)
                          setDevSecondaryAccent(c2)
                        } else {
                          setNormalAccent(c1)
                          setNormalSecondaryAccent(c2)
                        }
                        document.documentElement.style.setProperty('--color-accent', c1)
                        document.documentElement.style.setProperty('--color-accent-secondary', c2)
                        document.documentElement.style.setProperty('--color-accent-hover', darken(c1, 15))
                        addLog(`[Theme] accent = ${c1} / ${c2}`)
                      }}
                      className={`relative w-5 h-5 rounded-md overflow-hidden transition-all duration-150 ${disabled ? 'opacity-30 cursor-not-allowed' : ''}`}
                    >
                      <span className="absolute inset-0" style={{ background: c1 }} />
                      <span
                        className="absolute inset-0"
                        style={{
                          background: c2,
                          clipPath:
                            'polygon(87.5% 0%, 100% 0%, 100% 100%, 12.5% 100%, 31.25% 50%, 68.75% 50%)',
                        }}
                      />
                      {/* Internal cut line (SVG, 20×20) */}
                      <svg
                        className="absolute inset-0 w-full h-full pointer-events-none"
                        viewBox="0 0 20 20"
                        preserveAspectRatio="none"
                      >
                        <polyline
                          points="17.5,0 13.75,10 6.25,10 2.5,20"
                          fill="none"
                          stroke="var(--color-bg-secondary)"
                          strokeWidth="2"
                          strokeLinejoin="miter"
                        />
                      </svg>
                    </button>
                  </span>
                  </Tooltip>
                )
              })}
            </div>
          </div>
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">Log dir</label>
            <Tooltip text="日志文件存放路径" className="flex-1 min-w-0">
              <input
                value={logDir}
                readOnly
                className="w-full h-7 rounded-lg border border-border bg-bg-primary px-3 text-sm text-text-muted outline-none cursor-default font-mono text-xs truncate"
              />
            </Tooltip>
            <Tooltip text="修改日志目录">
              <button
                onClick={async () => {
                  try {
                    const res = await hostCall('pick_log_dir')
                    if (res?.dir) {
                      setLogDir(res.dir)
                      addLog(`[Setting] log dir = ${res.dir}`)
                    }
                  } catch (_) {}
                }}
                className="shrink-0 p-1.5 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors"
              >
                <Pencil className="w-4 h-4" />
              </button>
            </Tooltip>
            <Tooltip text="在资源管理器中打开日志目录">
              <button
                onClick={() => hostCall('open_log_dir').catch(() => {})}
                className="shrink-0 p-1.5 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors"
              >
                <FolderOpen className="w-4 h-4" />
              </button>
            </Tooltip>
          </div>
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">Keep files</label>
            <Tooltip text="Log 菜单中显示的历史日志文件数">
              <select
                value={keepFiles}
                onChange={(e) => setKeepFiles(Number(e.target.value))}
                className="h-7 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent"
              >
                {[3, 5, 7, 10].map((n) => (
                  <option key={n} value={n}>
                    {n} files
                  </option>
                ))}
              </select>
            </Tooltip>
          </div>
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">Dev mode</label>
            <Tooltip text="开发者模式 — 强制红色/黑客绿主题，解锁帧保存功能">
            <button
              onClick={() => {
                setDevMode(!devMode)
                addLog(`[Dev] ${!devMode ? 'ON' : 'OFF'}`)
              }}
              className={`relative w-10 h-5 rounded-full transition-colors ${devMode ? 'bg-accent-secondary' : 'bg-bg-tertiary'}`}
            >
              <span
                className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${devMode ? 'translate-x-5' : ''}`}
              />
            </button>
            </Tooltip>
          </div>
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">Mapping key</label>
            {recording ? (
              <>
                <span className={`h-7 px-3 rounded-lg border border-accent bg-accent/10 text-accent text-sm font-mono flex items-center ${displayCombo ? '' : 'animate-pulse'}`}>
                  {displayCombo || 'Press keys...'}
                </span>
                <span className="flex-1" />
                <Tooltip text="快捷键触发指示器 — 按下快捷键时闪烁绿光">
                <span className={`w-2.5 h-2.5 rounded-full shrink-0 transition-colors duration-100 ${triggerFlash ? 'bg-accent shadow-[0_0_6px_var(--color-accent)]' : 'bg-border'}`} />
                </Tooltip>
                <Tooltip text="取消修改，恢复原快捷键">
                <button
                  onClick={cancelRecording}
                  className="px-2.5 h-7 rounded-md text-xs font-medium border border-border text-text-secondary hover:bg-bg-hover transition-colors"
                >
                  Cancel
                </button>
                </Tooltip>
              </>
            ) : (
              <>
                <Tooltip text="当前快捷键，点击 Change 可修改">
                <span className="h-7 px-3 rounded-lg border border-border bg-bg-primary text-sm font-mono text-text-primary flex items-center min-w-[80px]">
                  {mappingHotkey}
                </span>
                </Tooltip>
                {/* ── Modifier-only warning badge ── */}
                {isModifierOnly && (
                  <Tooltip text="快捷键仅含修饰键（Ctrl/Alt/Shift/Win），可能与系统快捷键冲突。建议加入至少一个非修饰键，如字母、数字或 F1-F12。">
                  <span className="h-7 px-2 rounded-md text-xs font-medium text-accent-secondary bg-accent-secondary/10 border border-accent-secondary/30 flex items-center shrink-0 whitespace-nowrap">
                    ⚠ 纯修饰键
                  </span>
                  </Tooltip>
                )}
                <span className="flex-1" />
                <Tooltip text="快捷键触发指示器 — 按下快捷键时闪烁绿光">
                <span className={`w-2.5 h-2.5 rounded-full shrink-0 transition-colors duration-100 ${triggerFlash ? 'bg-accent shadow-[0_0_6px_var(--color-accent)]' : 'bg-border'}`} />
                </Tooltip>
                <Tooltip text="点击后按下新快捷键，松开所有按键确认">
                <button
                  onClick={startRecording}
                  className="px-2.5 h-7 rounded-md text-xs font-medium border border-border text-text-secondary hover:bg-bg-hover hover:text-text-primary transition-colors"
                >
                  Change
                </button>
                </Tooltip>
              </>
            )}
          </div>
        </div>
      </SettingsCard>

      <SettingsCard
        icon={<RefreshCw className="w-4 h-4 text-text-secondary" />}
        title="About"
      >
        <div className="space-y-3">
          <div className="flex items-center justify-between">
            <div>
              <div className="text-sm text-text-primary">Game Agent Monitor</div>
              <div className="text-xs text-text-muted">Version {appVersion}</div>
            </div>
            <ActionBtn
              icon={hasUpdate
                ? <RefreshCw className="w-3.5 h-3.5 text-accent" />
                : <RefreshCw className="w-3.5 h-3.5" />}
              label={hasUpdate ? 'Update Available' : 'Check Update'}
              title={hasUpdate ? '新版本可用，点击安装' : '检查新版本'}
              variant={hasUpdate ? 'primary' : 'outline'}
              onClick={onCheckUpdate || (() => addLog('[Action] check update'))}
            />
          </div>
          <div className="border-t border-border pt-2 flex items-center justify-between">
            <div className="text-xs text-text-muted min-w-0">
              <Tooltip text="在浏览器中打开项目 Gitee 页面">
              <button
                onClick={() => {
                  try {
                    window.open('https://gitee.com/Andyqwe44/tictactoe', '_blank')
                  } catch {}
                  addLog('[Project] open Gitee')
                }}
                className="text-accent hover:underline cursor-pointer truncate"
              >
                gitee.com/Andyqwe44/tictactoe
              </button>
              </Tooltip>
              <span className="mx-2 text-border hidden sm:inline">|</span>
              <span className="hidden sm:inline">
                C++ WebView2 · React · Tailwind · DXGI · WGC
              </span>
            </div>
            <ActionBtn
              icon={<span>★</span>}
              label="Star on GitHub"
              title="给项目点Star支持开发"
              variant="primary"
              onClick={() => {
                try {
                  window.open('https://github.com/Andyqwe44/tictactoe', '_blank')
                } catch {}
              }}
              className="shrink-0 ml-2"
            />
          </div>
        </div>
      </SettingsCard>

      <div className="h-4" />
    </div>
  )
}
