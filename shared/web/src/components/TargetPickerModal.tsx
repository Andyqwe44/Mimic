// ═══ Target Picker Modal ───
import { useState, useEffect } from 'react'
import { X, Search, RefreshCw, MonitorUp, Monitor, MonitorSmartphone, Smartphone } from 'lucide-react'
import { Tooltip } from './Toolkit'
import { useTranslation } from 'react-i18next'
import { hostCall, addLog } from '../lib/bridge'
import { DESKTOP_TITLE, displayTargetTitle } from '../lib/windowTitle'
import type { WindowInfo } from '../lib/types'
import { targetToWindowInfo, windowInfoToTarget } from '../lib/targets'
import { isAndroidHost } from '../lib/platform'
import { useScrollLock } from '../lib/useScrollLock'
import { MODAL_W } from '../lib/design'

const PICKER_W = MODAL_W.picker
const PICKER_MAXH = 'max-h-[min(560px,85vh)] min-h-[min(560px,85vh)]'

type FilterId = 'all' | 'desktop' | 'window' | 'display' | 'app'

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
  const { t } = useTranslation()
  const android = isAndroidHost()
  const [search, setSearch] = useState('')
  const [windows, setWindows] = useState<WindowInfo[]>([])
  const [filter, setFilter] = useState<FilterId>('all')
  const [loading, setLoading] = useState(false)
  const [currentDesktop, setCurrentDesktop] = useState(1)

  useScrollLock(open)

  useEffect(() => {
    if (open) {
      setFilter('all')
      setSearch('')
      loadTargets()
      if (!android) loadCurrentDesktop()
    }
  }, [open, android])

  const loadTargets = async () => {
    setLoading(true)
    try {
      // Prefer v2 list_targets; fall back to list_windows for older hosts.
      try {
        const res = await hostCall('list_targets')
        const arr = Array.isArray(res?.targets) ? res.targets : Array.isArray(res) ? res : null
        if (arr) {
          setWindows(arr.map((x: any) => targetToWindowInfo({
            id: String(x.id ?? ''),
            platform: x.platform ?? (android ? 'android' : 'windows'),
            kind: x.kind ?? 'window',
            title: String(x.title ?? ''),
            hwnd: x.hwnd,
            packageName: x.packageName,
            activity: x.activity,
            displayId: x.displayId,
            desktop: x.desktop,
            capabilities: x.capabilities,
          })))
          addLog(`[Target] loaded ${arr.length} via list_targets`)
          setLoading(false)
          return
        }
      } catch {
        /* fall through */
      }
      const list = await hostCall('list_windows')
      setWindows(Array.isArray(list) ? list : [])
      addLog(`[Window] loaded ${Array.isArray(list) ? list.length : 0} entries`)
    } catch {
      setWindows([
        { title: DESKTOP_TITLE, category: 'desktop', hwnd: 0, id: android ? 'display:0' : 'hwnd:0', platform: android ? 'android' : 'windows', kind: android ? 'display' : 'desktop' },
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

  const categories: FilterId[] = android
    ? ['all', 'display', 'app']
    : ['all', 'desktop', 'window']

  const filtered = windows
    .filter((w) => {
      const kind = w.kind ?? (w.category === 'desktop' || w.hwnd === 0 ? (android ? 'display' : 'desktop') : 'window')
      if (filter === 'all') return true
      if (filter === 'desktop' || filter === 'display') return kind === 'desktop' || kind === 'display' || w.category === 'desktop'
      if (filter === 'app') return kind === 'app' || !!w.packageName
      if (filter === 'window') return kind === 'window' || w.category === 'window'
      return true
    })
    .filter((w) => w.title.toLowerCase().includes(search.toLowerCase()))

  const handlePickWindow = async (w: WindowInfo) => {
    onSelectWindow(w)
    const target = windowInfoToTarget(w)
    addLog(`[Target] ${target.id} ${w.title}`)

    if (!android) {
      const winDesktop = w.desktop || 1
      if (w.desktop != null && winDesktop !== currentDesktop && w.category !== 'desktop') {
        try {
          await hostCall('switch_desktop', { index: winDesktop - 1 })
          addLog(`[Desktop] switched to D${winDesktop}`)
        } catch {
          /* continue */
        }
      }
    }

    // Android: MediaProjection path — no WGC/DXGI mode page.
    if (android) {
      if (target.kind === 'app' && target.packageName) {
        try {
          await hostCall('launch_app', {
            packageName: target.packageName,
            activity: target.activity ?? '',
          })
          addLog(`[Launch] ${target.packageName}`)
        } catch (e: any) {
          addLog(`[Launch] failed: ${e?.message || e}`)
        }
      }
      onSelectMode('mediaprojection', target.kind === 'app' ? 'app' : 'display')
      onClose()
      return
    }

    if (w.hwnd === 0 || w.category === 'desktop') {
      onSelectMode('dxgi', 'desktop')
    } else {
      onSelectMode('wgc', 'foreground')
    }
    onClose()
  }

  if (!open) return null

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center">
      <div
        className="absolute inset-0 bg-scrim"
        onClick={() => {
          onClose()
          addLog('[Target] picker cancelled')
        }}
      />
      <div
        className={`relative ${PICKER_W} ${PICKER_MAXH} bg-bg-secondary border border-border rounded-xl shadow-lg overflow-hidden flex flex-col`}
      >
        <div className="flex items-center justify-between px-4 py-3 border-b border-border">
          <div className="flex items-center gap-2">
            <MonitorUp className="w-4 h-4 text-accent" />
            <span className="text-sm font-semibold text-text-primary">{t('targetPicker.select_target')}</span>
          </div>
          <Tooltip text={t('common.close')}>
            <button
              onClick={() => {
                onClose()
                addLog('[Target] picker cancelled')
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
              text={
                c === 'all'
                  ? t('targetPicker.filter_all')
                  : c === 'desktop'
                    ? t('targetPicker.filter_desktop')
                    : c === 'display'
                      ? t('targetPicker.filter_display')
                      : c === 'app'
                        ? t('targetPicker.filter_app')
                        : t('targetPicker.filter_window')
              }
            >
              <button
                onClick={() => {
                  setFilter(c)
                  addLog(`[Filter] ${c}`)
                }}
                className={`px-3 py-1 rounded-full text-xs font-medium transition-colors capitalize
                  ${filter === c ? 'bg-accent text-white' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'}`}
              >
                {c === 'all'
                  ? t('targetPicker.all')
                  : c === 'desktop'
                    ? t('targetPicker.desktop')
                    : c === 'display'
                      ? t('targetPicker.display')
                      : c === 'app'
                        ? t('targetPicker.apps')
                        : t('targetPicker.windows')}
              </button>
            </Tooltip>
          ))}
          <div className="flex-1" />
          <Tooltip text={t('targetPicker.refresh')}>
            <button
              onClick={() => {
                loadTargets()
                addLog('[Target] refreshing list')
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
              placeholder={t('targetPicker.search')}
              className="flex-1 bg-transparent text-sm text-text-primary outline-none placeholder:text-text-muted"
              autoFocus
            />
          </div>
        </div>
        <div className="flex-1 overflow-y-auto px-2 pb-2 min-h-0">
          {loading && filtered.length === 0 ? (
            <div className="flex items-center justify-center py-8 text-sm text-text-muted">
              {t('common.loading')}
            </div>
          ) : (
            <div className="grid grid-cols-2 gap-1">
              {filtered.map((w) => {
                const target = windowInfoToTarget(w)
                const self =
                  w.title.includes('Game Agent Monitor') ||
                  w.title.includes('Mimic Client') ||
                  w.packageName === 'com.mimic.client'
                const winDesktop = w.desktop || 1
                const isRemote =
                  !android &&
                  w.desktop != null &&
                  winDesktop !== currentDesktop &&
                  w.category !== 'desktop'
                const kindLabel =
                  target.kind === 'display'
                    ? t('targetPicker.display')
                    : target.kind === 'app'
                      ? t('targetPicker.apps')
                      : target.kind === 'desktop'
                        ? t('connection.category_desktop')
                        : t('connection.category_window')
                return (
                  <Tooltip
                    key={w.id ?? `${w.hwnd}-${w.category}-${w.packageName ?? ''}`}
                    text={
                      self
                        ? t('targetPicker.self_window_tip')
                        : isRemote
                          ? t('targetPicker.select_remote_tip', { title: displayTargetTitle(w.title, t), n: winDesktop })
                          : t('targetPicker.select_tip', { title: displayTargetTitle(w.title, t) })
                    }
                  >
                    <button
                      disabled={self}
                      onClick={() => handlePickWindow(w)}
                      className={`w-full flex items-center gap-2 px-2 py-2 rounded-lg text-left transition-colors group min-w-0 relative
                        ${self ? 'opacity-40 cursor-not-allowed' : 'hover:bg-bg-hover cursor-pointer'}`}
                    >
                      {target.kind === 'display' || target.kind === 'desktop' ? (
                        <MonitorSmartphone className="w-3.5 h-3.5 text-text-muted group-hover:text-accent shrink-0" />
                      ) : target.kind === 'app' ? (
                        <Smartphone className="w-3.5 h-3.5 text-text-muted group-hover:text-accent shrink-0" />
                      ) : (
                        <Monitor className="w-3.5 h-3.5 text-text-muted group-hover:text-accent shrink-0" />
                      )}
                      <div className="flex-1 min-w-0">
                        <span className="text-xs text-text-primary truncate block">
                          {displayTargetTitle(w.title, t)}
                        </span>
                        <span className="text-xs text-text-muted capitalize truncate block">
                          {kindLabel}
                          {w.packageName ? ` · ${w.packageName}` : ''}
                        </span>
                      </div>
                      {!android && (w.desktop != null || w.category !== 'desktop') && (
                        <span className="absolute bottom-1 right-1 flex items-center gap-0.5 text-[10px] font-medium px-1.5 py-px rounded-full bg-accent-light text-accent whitespace-nowrap">
                          {isRemote && <span>⚡</span>}
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
          {android
            ? t('targetPicker.footer_android', {
                items: filtered.length,
                displays: windows.filter((w) => (w.kind ?? w.category) === 'display' || w.category === 'desktop').length,
                apps: windows.filter((w) => w.kind === 'app' || !!w.packageName).length,
              })
            : (() => {
                const deskNums = new Set(
                  windows.filter((w) => (w.desktop || 0) > 0).map((w) => w.desktop || 1),
                )
                return t('targetPicker.footer_summary', {
                  items: filtered.length,
                  desks: deskNums.size || 1,
                  wins: windows.filter((w) => w.category === 'window').length,
                })
              })()}
        </div>
      </div>
    </div>
  )
}
