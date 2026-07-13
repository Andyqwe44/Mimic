// ═══ Target Picker Modal ───
import { useState, useEffect } from 'react'
import { X, Search, ChevronLeft, RefreshCw, MonitorUp, Monitor, MonitorSmartphone } from 'lucide-react'
import { Tooltip } from './Toolkit'
import { hostCall, addLog } from '../lib/bridge'
import { CAPTURE_MODES } from '../lib/constants'
import type { WindowInfo } from '../lib/types'
import { useScrollLock } from '../lib/useScrollLock'
import { MODAL_W } from '../lib/design'

const PICKER_W = MODAL_W.picker
const PICKER_MAXH = 'max-h-[min(560px,85vh)] min-h-[min(560px,85vh)]'

export function TargetPickerModal({
  open,
  onClose,
  onSelectWindow,
  onSelectMode,
}: {
  open: boolean
  onClose: () => void
  onSelectWindow: (w: WindowInfo) => void
  onSelectMode: (method: string, expectedState: string) => void
}) {
  const [page, setPage] = useState<'window' | 'mode'>('window')
  const [animReady, setAnimReady] = useState(false)
  const [search, setSearch] = useState('')
  const [windows, setWindows] = useState<WindowInfo[]>([])
  const [filter, setFilter] = useState('all')
  const [loading, setLoading] = useState(false)
  const [currentDesktop, setCurrentDesktop] = useState(1)
  const [pendingWin, _setPendingWin] = useState<WindowInfo | null>(null)

  // Lock body scroll while modal is open
  useScrollLock(open)

  useEffect(() => {
    if (open) {
      setPage('window')
      setFilter('all')
      setSearch('')
      loadWindows()
      loadCurrentDesktop()
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
      setWindows([
        { title: ' Entire Desktop', category: 'desktop', hwnd: 0 },
        { title: 'Tic Tac Toe — main.exe', category: 'window', hwnd: 0 },
        { title: 'Notepad', category: 'window', hwnd: 0 },
        { title: 'Chrome', category: 'window', hwnd: 0 },
      ])
    }
    setLoading(false)
  }

  const loadCurrentDesktop = async () => {
    try {
      const list = await hostCall('list_desktops')
      const cur = list.find((d: any) => d.current)
      if (cur) setCurrentDesktop(cur.index)
    } catch {
      /* use default 1 */
    }
  }

  const categories = ['all', 'desktop', 'window'] as const
  const filtered = windows
    .filter((w) => {
      if (filter === 'desktop') return w.category === 'desktop'
      if (filter === 'window') return w.category === 'window'
      return true
    })
    .filter((w) => w.title.toLowerCase().includes(search.toLowerCase()))

  const handlePickWindow = async (w: WindowInfo) => {
    onSelectWindow(w)
    addLog(`[Window] ${w.title}`)
    const winDesktop = w.desktop || 1
    if (w.desktop != null && winDesktop !== currentDesktop && w.category !== 'desktop') {
      try {
        await hostCall('switch_desktop', { index: winDesktop - 1 })
        addLog(`[Desktop] switched to D${winDesktop}`)
      } catch {
        /* continue anyway */
      }
    }
    if (w.hwnd === 0 || w.category === 'desktop') {
      onSelectMode('dxgi', 'desktop')
      onClose()
    } else {
      onSelectMode('wgc', 'foreground')
      onClose()
    }
  }

  const handlePickMode = (method: string, expectedState: string) => {
    onSelectMode(method, expectedState)
    onClose()
  }

  if (!open) return null

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center">
      <div
        className="absolute inset-0 bg-black/30"
        onClick={() => {
          onClose()
          addLog('[Window] picker cancelled')
        }}
      />
      <div
        className={`relative ${PICKER_W} ${PICKER_MAXH} bg-bg-secondary border border-border rounded-xl shadow-lg overflow-hidden`}
      >
        <div
          className={`flex ${animReady ? 'transition-transform duration-300 ease-out' : ''}`}
          style={{
            transform: page === 'window' ? 'translateX(0)' : 'translateX(-100%)',
          }}
        >
          {/* Page 1: Window Picker */}
          <div className={`${PICKER_W} flex-shrink-0 flex flex-col ${PICKER_MAXH}`}>
            <div className="flex items-center justify-between px-4 py-3 border-b border-border">
              <div className="flex items-center gap-2">
                <MonitorUp className="w-4 h-4 text-accent" />
                <span className="text-sm font-semibold text-text-primary">Select Target</span>
              </div>
              <Tooltip text="关闭">
                <button
                  onClick={() => {
                    onClose()
                    addLog('[Window] picker cancelled')
                  }}
                  className="p-1 rounded-md hover:bg-bg-hover transition-colors"
                >
                  <X className="w-4 h-4 text-text-secondary" />
                </button>
              </Tooltip>
            </div>
            <div className="flex items-center gap-1 px-4 pt-3 pb-1">
              {categories.map((c) => (
                <Tooltip
                  key={c}
                  text={`筛选: ${c === 'all' ? '全部' : c === 'desktop' ? '桌面' : '窗口'}`}
                >
                  <button
                    onClick={() => {
                      setFilter(c)
                      addLog(`[Filter] ${c}`)
                    }}
                    className={`px-3 py-1 rounded-full text-xs font-medium transition-colors capitalize
                      ${filter === c ? 'bg-accent text-white' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'}`}
                  >
                    {c === 'all' ? 'All' : c === 'desktop' ? ' Desktop' : ' Windows'}
                  </button>
                </Tooltip>
              ))}
              <div className="flex-1" />
              <Tooltip text="刷新窗口列表">
                <button
                  onClick={() => {
                    loadWindows()
                    addLog('[Window] refreshing list')
                  }}
                  className={`p-1.5 rounded-md hover:bg-bg-hover transition-colors text-text-secondary ${loading ? 'animate-spin' : ''}`}
                >
                  <RefreshCw className="w-3.5 h-3.5" />
                </button>
              </Tooltip>
            </div>
            <div className="px-4 py-2">
              <div className="flex items-center gap-2 h-7 rounded-lg border border-border bg-bg-primary px-3">
                <Search className="w-3.5 h-3.5 text-text-muted shrink-0" />
                <input
                  value={search}
                  onChange={(e) => setSearch(e.target.value)}
                  placeholder="Search..."
                  className="flex-1 bg-transparent text-sm text-text-primary outline-none placeholder:text-text-muted"
                  autoFocus
                />
              </div>
            </div>
            <div className="flex-1 overflow-y-auto px-2 pb-2">
              {loading && filtered.length === 0 ? (
                <div className="flex items-center justify-center py-8 text-sm text-text-muted">
                  Loading...
                </div>
              ) : (
                <div className="grid grid-cols-2 gap-1">
                  {filtered.map((w) => {
                    const self = w.title.includes('Game Agent Monitor')
                    const winDesktop = w.desktop || 1
                    const isRemote =
                      w.desktop != null &&
                      winDesktop !== currentDesktop &&
                      w.category !== 'desktop'
                    return (
                      <Tooltip
                        key={`${w.hwnd}-${w.category}`}
                        text={
                          self
                            ? '自身窗口，禁止捕获'
                            : isRemote
                              ? `选择: ${w.title}（将切换到 D${winDesktop}）`
                              : `选择: ${w.title}`
                        }
                      >
                        <button
                          disabled={self}
                          onClick={() => handlePickWindow(w)}
                          className={`w-full flex items-center gap-2 px-2 py-2 rounded-lg text-left transition-colors group min-w-0 relative
                            ${self ? 'opacity-40 cursor-not-allowed' : 'hover:bg-bg-hover cursor-pointer'}`}
                        >
                          {w.category === 'desktop' ? (
                            <MonitorSmartphone className="w-3.5 h-3.5 text-text-muted group-hover:text-accent shrink-0" />
                          ) : (
                            <Monitor className="w-3.5 h-3.5 text-text-muted group-hover:text-accent shrink-0" />
                          )}
                          <div className="flex-1 min-w-0">
                            <span className="text-xs text-text-primary truncate block">
                              {w.title}
                            </span>
                            <span className="text-xs text-text-muted capitalize">
                              {w.category}
                            </span>
                          </div>
                          {(w.desktop != null || w.category !== 'desktop') && (
                            <span className="absolute bottom-1 right-1 flex items-center gap-0.5 text-[10px] font-medium px-1.5 py-px rounded-full bg-accent-light text-accent whitespace-nowrap">
                              {isRemote && (
                                <Tooltip text="需切换桌面">
                                  <span>⚡</span>
                                </Tooltip>
                              )}
                              D{winDesktop || '?'}
                            </span>
                          )}
                        </button>
                      </Tooltip>
                    )
                  })}
                </div>
              )}
            </div>
            <div className="px-4 py-2 border-t border-border text-xs text-text-muted text-center">
              {(() => {
                const deskNums = new Set(
                  windows
                    .filter((w) => (w.desktop || 0) > 0)
                    .map((w) => w.desktop || 1),
                )
                const deskCount = deskNums.size || 1
                return `${filtered.length} items — ${deskCount} desktop${deskCount !== 1 ? 's' : ''}, ${windows.filter((w) => w.category === 'window').length} windows`
              })()}
            </div>
          </div>

          {/* Page 2: Capture Mode */}
          <div className={`${PICKER_W} flex-shrink-0 flex flex-col`}>
            <div className="flex items-center justify-between px-4 py-3 border-b border-border">
              <div className="flex items-center gap-2">
                <Tooltip text="返回窗口选择">
                  <button
                    onClick={() => setPage('window')}
                    className="p-1 rounded-md hover:bg-bg-hover transition-colors"
                  >
                    <ChevronLeft className="w-4 h-4 text-text-secondary" />
                  </button>
                </Tooltip>
                <MonitorUp className="w-4 h-4 text-accent" />
                <span className="text-sm font-semibold text-text-primary">Capture Mode</span>
              </div>
              <Tooltip text="关闭">
                <button
                  onClick={() => {
                    onClose()
                    addLog('[Capture] mode picker cancelled')
                  }}
                  className="p-1 rounded-md hover:bg-bg-hover transition-colors"
                >
                  <X className="w-4 h-4 text-text-secondary" />
                </button>
              </Tooltip>
            </div>
            <div className="p-4 space-y-2">
              <div className="text-xs text-text-muted mb-3">
                目标:{' '}
                <span className="text-text-primary font-medium">
                  {pendingWin?.title || ''}
                </span>
              </div>
              <div className="text-xs text-text-muted mb-2">
                请选择目标窗口的当前状态，系统将自动推荐最优捕获方案：
              </div>
              {CAPTURE_MODES.map((m) => (
                <Tooltip key={m.v} text={m.desc}>
                <button
                  onClick={() => handlePickMode(m.method, m.v)}
                  className="w-full flex flex-col items-start gap-0.5 px-3 py-2.5 rounded-lg text-left transition-colors hover:bg-bg-hover border border-border"
                >
                  <span className="text-sm font-medium text-text-primary">{m.label}</span>
                  <span className="text-xs text-text-muted">{m.desc}</span>
                </button>
                </Tooltip>
              ))}
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}
