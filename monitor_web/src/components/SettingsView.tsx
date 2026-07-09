// ═══ Settings View ───
import { useState, useEffect } from 'react'
import {
  Camera, Play, Cpu, Sun, RefreshCw, ChevronDown,
  Monitor, Pencil, FolderOpen,
} from 'lucide-react'
import { Tooltip, ActionBtn } from './Toolkit'
import { ConnectionPanel } from './ConnectionPanel'
import { hostCall, addLog } from '../lib/bridge'
import {
  COLLAPSIBLE_HEADER, SELECTABLE_BTN, CAPTURE_METHODS, RENDER_METHODS, INPUT_METHODS,
} from '../lib/constants'
import type { WindowInfo } from '../lib/types'

// ── SettingsCard (collapsible) ──
function SettingsCard({
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
  saveCaptureFrames, setSaveCaptureFrames,
  saveStreamFrames, setSaveStreamFrames,
  frameDumpDir, setFrameDumpDir,
  inputMethod, setInputMethod,
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
  saveCaptureFrames: boolean; setSaveCaptureFrames: (v: boolean) => void
  saveStreamFrames: boolean; setSaveStreamFrames: (v: boolean) => void
  frameDumpDir: string; setFrameDumpDir: (d: string) => void
  inputMethod: string; setInputMethod: (m: string) => void
}) {
  const colors = ['#3B82F6', '#8B5CF6', '#EC4899', '#F59E0B', '#10B981', '#EF4444']
  const [accent, setAccent] = useState('#3B82F6')
  const [screenRes, setScreenRes] = useState('?×?')
  const [logDir, setLogDir] = useState('...')
  const [connExpanded, setConnExpanded] = useState(true)

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
                    className={`relative w-10 h-5 rounded-full transition-colors ${autoSnap ? 'bg-amber-500' : 'bg-bg-tertiary'}`}
                  >
                    <span
                      className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${autoSnap ? 'translate-x-5' : ''}`}
                    />
                  </button>
                </label>
              </div>
              <div className="flex flex-col gap-2">
                {CAPTURE_METHODS.map((m) => {
                  const isActive = snapMethod === m.v
                  const ringClass = !autoSnap && isActive
                    ? 'border-accent bg-accent/10 cursor-pointer'
                    : autoSnap && isActive
                      ? 'border-amber-500 bg-amber-500/10 cursor-not-allowed'
                      : autoSnap
                        ? 'border-border bg-bg-primary opacity-50 cursor-not-allowed'
                        : 'border-border bg-bg-primary hover:bg-bg-hover cursor-pointer'
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
                              className="text-[11px] font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded"
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
                  <button
                    onClick={(e) => {
                      e.stopPropagation()
                      const next = !autoStream
                      setAutoStream?.(next)
                      if (next) setStreamMethod('wgc')
                      addLog(`[Setting] auto stream = ${next}`)
                    }}
                    className={`relative w-10 h-5 rounded-full transition-colors ${autoStream ? 'bg-amber-500' : 'bg-bg-tertiary'}`}
                  >
                    <span
                      className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${autoStream ? 'translate-x-5' : ''}`}
                    />
                  </button>
                </label>
              </div>
              <div className="flex flex-col gap-2">
                {CAPTURE_METHODS.map((m) => {
                  const unsupported = m.v === 'dxgi'
                  const isActive = streamMethod === m.v
                  const ringClass =
                    !autoStream && isActive && !unsupported
                      ? 'border-accent bg-accent/10 cursor-pointer'
                      : autoStream && isActive
                        ? 'border-amber-500 bg-amber-500/10 cursor-not-allowed'
                        : autoStream || unsupported
                          ? 'border-border bg-bg-primary opacity-50 cursor-not-allowed'
                          : 'border-border bg-bg-primary hover:bg-bg-hover cursor-pointer'
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
                          disabled={autoStream || unsupported}
                          onChange={(e) => {
                            if (!autoStream && !unsupported) {
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
                                className="text-[11px] font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded"
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
              <span className="text-xs text-text-muted">Render Method (🎨)</span>
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
          {/* ── Input Method (mouse + keyboard forwarding) ── */}
          <div className="border-t border-border pt-3 space-y-2">
            <div className="flex items-center justify-between">
              <span className="text-xs text-text-muted">Input Method (🖱 + ⌨)</span>
              <span className="text-[10px] text-text-muted">Monitor 预览区支持：单击/双击/拖拽/滚轮/键盘/组合键</span>
            </div>
            <div className="flex flex-col gap-2">
              {INPUT_METHODS.map((m) => {
                const isActive = inputMethod === m.v
                const implemented = m.v !== 'driver'
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
                        type="radio" name="inputMethod" value={m.v}
                        checked={isActive} disabled={!implemented}
                        onChange={(e) => {
                          if (implemented) {
                            setInputMethod(e.target.value)
                            addLog(`[Setting] input method = ${e.target.value}`)
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
          {/* ── Keyboard forwarding ── */}
          <div className="border-t border-border pt-3 space-y-2">
            <div className="flex items-center justify-between">
              <span className="text-xs text-text-muted">Keyboard (⌨)</span>
              <span className="text-[10px] text-text-muted">点击预览画面获取焦点后，键盘输入转发到目标窗口</span>
            </div>
            <div className="text-[11px] text-text-muted space-y-1">
              <div>• 普通按键 — <code className="text-accent bg-accent/10 px-1 rounded">keydown</code> / <code className="text-accent bg-accent/10 px-1 rounded">keyup</code></div>
              <div>• 组合键 — 自动识别为 <code className="text-accent bg-accent/10 px-1 rounded">combo</code>（Ctrl+C 等）</div>
              <div>• <code className="text-accent bg-accent/10 px-1 rounded">Esc</code> 或外部点击 → 释放焦点，自动松开所有已按下按键</div>
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
              className="w-full h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent"
            />
          </Tooltip>
        </div>
        <div className="flex items-center gap-3">
          <label className="text-sm text-text-secondary w-24 shrink-0">Adapter</label>
          <Tooltip text="游戏微调权重" className="flex-1 min-w-0">
            <input
              defaultValue="tictactoe-finetune"
              onBlur={(e) => addLog(`[Setting] adapter = ${e.target.value}`)}
              className="w-full h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent"
            />
          </Tooltip>
        </div>
      </SettingsCard>

      <SettingsCard icon={<Sun className="w-4 h-4 text-text-secondary" />} title="General">
        <div className="space-y-3">
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">Theme</label>
            <div className="flex gap-1">
              {[
                ['Light', 'light'],
                ['Dark', 'dark'],
                ['System', 'system'],
              ].map(([l, v]) => (
                <button
                  key={v}
                  onClick={() => {
                    setTheme(v as 'light' | 'dark' | 'system')
                    addLog(`[Theme] ${l}`)
                  }}
                  className={`px-3 py-1 rounded-full text-xs font-medium transition-colors ${theme === v ? 'bg-accent text-white' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'}`}
                >
                  {l}
                </button>
              ))}
            </div>
          </div>
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">Accent</label>
            <div className="flex gap-1.5">
              {colors.map((c) => (
                <button
                  key={c}
                  onClick={() => {
                    setAccent(c)
                    document.documentElement.style.setProperty('--color-accent', c)
                    addLog(`[Theme] accent = ${c}`)
                  }}
                  className="w-6 h-6 rounded-full transition-all duration-150"
                  style={{
                    background: c,
                    boxShadow:
                      accent === c
                        ? `0 0 0 2px var(--color-bg-secondary), 0 0 0 4px ${c}`
                        : 'none',
                  }}
                />
              ))}
            </div>
          </div>
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">Log dir</label>
            <Tooltip text="日志文件存放路径" className="flex-1 min-w-0">
              <input
                value={logDir}
                readOnly
                className="w-full h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm text-text-muted outline-none cursor-default font-mono text-xs truncate"
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
                className="h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm outline-none focus:border-accent"
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
            <button
              onClick={() => {
                setDevMode(!devMode)
                addLog(`[Dev] ${!devMode ? 'ON' : 'OFF'}`)
              }}
              className={`relative w-10 h-5 rounded-full transition-colors ${devMode ? 'bg-amber-500' : 'bg-bg-tertiary'}`}
            >
              <span
                className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${devMode ? 'translate-x-5' : ''}`}
              />
            </button>
          </div>
        </div>
      </SettingsCard>

      {devMode && (
        <SettingsCard
          icon={<Cpu className="w-4 h-4 text-amber-400" />}
          title="Developer Mode"
          defaultExpanded={true}
        >
          <div className="space-y-3">
            <div className="flex items-center justify-between">
              <div>
                <div className="text-sm text-text-primary">Save single-frame captures</div>
                <div className="text-xs text-text-muted">
                  Save each 📷 snapshot as PNG to disk
                </div>
              </div>
              <button
                onClick={() => {
                  const v = !saveCaptureFrames
                  setSaveCaptureFrames(v)
                  hostCall('set_frame_dump', {
                    capture: v,
                    stream: saveStreamFrames,
                    dir: frameDumpDir,
                  }).catch(() => {})
                }}
                className={`relative w-10 h-5 rounded-full transition-colors ${saveCaptureFrames ? 'bg-success' : 'bg-bg-tertiary'}`}
              >
                <span
                  className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${saveCaptureFrames ? 'translate-x-5' : ''}`}
                />
              </button>
            </div>
            <div className="flex items-center justify-between">
              <div>
                <div className="text-sm text-text-primary">Save live preview frames</div>
                <div className="text-xs text-text-muted">
                  Save each ▶ preview frame as PNG to disk
                </div>
              </div>
              <button
                onClick={() => {
                  const v = !saveStreamFrames
                  setSaveStreamFrames(v)
                  hostCall('set_frame_dump', {
                    capture: saveCaptureFrames,
                    stream: v,
                    dir: frameDumpDir,
                  }).catch(() => {})
                }}
                className={`relative w-10 h-5 rounded-full transition-colors ${saveStreamFrames ? 'bg-success' : 'bg-bg-tertiary'}`}
              >
                <span
                  className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${saveStreamFrames ? 'translate-x-5' : ''}`}
                />
              </button>
            </div>
            <div className="flex items-center gap-2">
              <label className="text-sm text-text-secondary w-24 shrink-0">Dump dir</label>
              <Tooltip text="帧保存路径" className="flex-1 min-w-0">
                <input
                  value={frameDumpDir || '(not set)'}
                  readOnly
                  className="w-full h-8 rounded-lg border border-border bg-bg-primary px-3 text-sm text-text-muted outline-none cursor-default font-mono text-xs truncate"
                />
              </Tooltip>
              <Tooltip text="选择保存目录">
                <button
                  onClick={async () => {
                    try {
                      const res = await hostCall('pick_dir')
                      if (res?.dir) {
                        setFrameDumpDir(res.dir)
                        if (!saveCaptureFrames) setSaveCaptureFrames(true)
                        if (!saveStreamFrames) setSaveStreamFrames(true)
                        hostCall('set_frame_dump', {
                          capture: true,
                          stream: true,
                          dir: res.dir,
                        }).catch(() => {})
                        addLog(`[Dev] dump dir = ${res.dir}`)
                      }
                    } catch (_) {}
                  }}
                  className="shrink-0 p-1.5 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors"
                >
                  <Pencil className="w-4 h-4" />
                </button>
              </Tooltip>
              <Tooltip text="在资源管理器中打开保存目录">
                <button
                  onClick={() => {
                    if (frameDumpDir)
                      hostCall('open_dir', { dir: frameDumpDir }).catch(() => {})
                  }}
                  className="shrink-0 p-1.5 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors"
                >
                  <FolderOpen className="w-4 h-4" />
                </button>
              </Tooltip>
            </div>
          </div>
        </SettingsCard>
      )}

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
              icon={<RefreshCw className="w-3.5 h-3.5" />}
              label="Check Update"
              title="检查新版本"
              variant="outline"
              onClick={() => addLog('[Action] check update')}
            />
          </div>
          <div className="border-t border-border pt-2 flex items-center justify-between">
            <div className="text-xs text-text-muted min-w-0">
              <button
                onClick={() => {
                  try {
                    window.open('https://github.com/Andyqwe44/tictactoe', '_blank')
                  } catch {}
                  addLog('[Project] open GitHub')
                }}
                className="text-accent hover:underline cursor-pointer truncate"
              >
                github.com/Andyqwe44/tictactoe
              </button>
              <span className="mx-2 text-border hidden sm:inline">|</span>
              <span className="hidden sm:inline">
                C++ WebView2 · React · Tailwind · DXGI · WGC
              </span>
            </div>
            <Tooltip text="给项目点Star支持开发">
              <button
                onClick={() => {
                  try {
                    window.open('https://github.com/Andyqwe44/tictactoe', '_blank')
                  } catch {}
                }}
                className="inline-flex items-center gap-1.5 rounded-md px-2.5 h-7 text-xs font-medium bg-accent text-white hover:bg-accent-hover transition-colors shrink-0 ml-2"
              >
                <span>★</span>Star
              </button>
            </Tooltip>
          </div>
        </div>
      </SettingsCard>

      <div className="h-4" />
    </div>
  )
}
