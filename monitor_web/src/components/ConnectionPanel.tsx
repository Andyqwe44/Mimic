// ═══ Connection Panel — target selection + controller_server connect ═══
import { useState, useEffect } from 'react'
import { ChevronDown, MonitorUp, Unlink, Pin, Cable, Unplug } from 'lucide-react'
import { Tooltip, ActionBtn } from './Toolkit'
import { TargetPickerModal } from './TargetPickerModal'
import { addLog } from '../lib/bridge'
import {
  COLLAPSIBLE_HEADER,
  METHOD_SHORT,
  STATE_LABEL,
  cantCaptureMinimized,
} from '../lib/constants'
import { DESKTOP_TITLE, displayTargetTitle, isDesktopTitle } from '../lib/windowTitle'
import { useTranslation } from 'react-i18next'
import type { WindowInfo } from '../lib/types'

/** Shared left-column width: title+unlink ≡ ip+port (aligns with ActionBtn on the right). */
const LEFT_COL = 'w-[11.5rem]'

export function ConnectionPanel({
  onSelect,
  onDisconnect,
  snapMethod: _snapMethod,
  setSnapMethod,
  streamMethod,
  setStreamMethod,
  selWin,
  winState,
  expectedCaptureState: _expectedCaptureState,
  setExpectedCaptureState,
  expanded,
  onToggle,
  pinned,
  onTogglePin,
  serverHost,
  serverPort,
  onServerHostChange,
  onServerPortChange,
  serverConnected,
  onToggleServer,
}: {
  onSelect: (w: WindowInfo) => void
  onDisconnect?: () => void
  snapMethod: string
  setSnapMethod?: (m: string) => void
  streamMethod: string
  setStreamMethod?: (m: string) => void
  selWin?: WindowInfo
  winState: string
  expectedCaptureState?: string
  setExpectedCaptureState?: (s: string) => void
  expanded: boolean
  onToggle: () => void
  pinned?: boolean
  onTogglePin?: () => void
  serverHost: string
  serverPort: string
  onServerHostChange: (v: string) => void
  onServerPortChange: (v: string) => void
  serverConnected: boolean
  onToggleServer: () => void
}) {
  const { t } = useTranslation()

  const [pickerOpen, setPickerOpen] = useState(false)
  const [selTitle, setSelTitle] = useState(DESKTOP_TITLE)
  const isDesktop = isDesktopTitle(selTitle) || selWin?.category === 'desktop' || selWin?.hwnd === 0

  const handleSelectWindow = (w: WindowInfo) => {
    setSelTitle(w.title)
    onSelect(w)
  }

  const handleSelectMode = (method: string, expectedState: string) => {
    if (setSnapMethod) setSnapMethod(method)
    if (setStreamMethod) setStreamMethod(method)
    if (setExpectedCaptureState) setExpectedCaptureState(expectedState)
  }

  useEffect(() => {
    if (selWin && selWin.title !== selTitle) {
      setSelTitle(selWin.title)
    }
  }, [selWin?.title])

  const cantCapture = !isDesktop && cantCaptureMinimized(streamMethod, winState)
  const recommendedMethod = winState === 'minimized' ? 'dxgi' : 'wgc'

  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
      <div
        role="button"
        tabIndex={0}
        onClick={() => {
          onToggle()
          addLog(`[Connection] ${!expanded ? 'expanded' : 'collapsed'}`)
        }}
        onKeyDown={(e) => {
          if (e.key === 'Enter' || e.key === ' ') {
            ;(e.currentTarget as HTMLElement).click()
          }
        }}
        className={COLLAPSIBLE_HEADER}
      >
        <div className="flex items-center gap-2 min-w-0">
          <span className="w-5 h-5 rounded bg-blue-400/15 flex items-center justify-center shrink-0">
            <MonitorUp className="w-3 h-3 text-blue-400" />
          </span>
          <span className="text-sm font-medium text-text-primary shrink-0">{t('connection.title')}</span>
          <span className="text-[11px] font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded shrink-0">
            {t(STATE_LABEL[winState] || winState)}
          </span>
          <span className="text-[11px] font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded shrink-0">
            {METHOD_SHORT[recommendedMethod] || recommendedMethod}
          </span>
          {serverConnected && (
            <span className="text-[11px] font-medium text-emerald-500 bg-emerald-500/10 px-1.5 py-0.5 rounded shrink-0">
              {t('connection.server_on')}
            </span>
          )}
        </div>
        <div className="flex items-center gap-2 ml-2">
          {cantCapture && <span className="text-xs text-error shrink-0">⚠</span>}
          {onTogglePin && (
            <Tooltip text={pinned ? t('connection.unpin_tip') : t('connection.pin_tip')}>
              <button
                onClick={(e) => {
                  e.stopPropagation()
                  onTogglePin()
                }}
                className={`p-1 rounded-md transition-colors ${pinned ? 'text-accent hover:bg-bg-tertiary' : 'text-text-secondary hover:text-text-primary hover:bg-bg-tertiary'}`}
              >
                <Pin className={`w-3.5 h-3.5 ${pinned ? 'fill-current' : ''}`} />
              </button>
            </Tooltip>
          )}
          <ChevronDown
            className={`w-4 h-4 text-text-muted transition-transform duration-150 ${expanded ? 'rotate-180' : ''}`}
          />
        </div>
      </div>
      <div
        className="grid transition-[grid-template-rows] duration-150 ease-out"
        style={{ gridTemplateRows: expanded ? '1fr' : '0fr' }}
      >
        <div className="overflow-hidden min-h-0" data-layout-measure="">
          <div className="border-t border-border" />
          <div className="max-h-[360px] overflow-y-auto p-3 space-y-2">
            {cantCapture && (
              <div className="text-xs text-error bg-red-500/10 rounded-lg px-2 py-1.5">
                {t('connection.minimized_warning', { method: streamMethod.toUpperCase() })}
              </div>
            )}
            <div className="flex justify-between items-center gap-2">
              <div className={`flex items-center gap-1.5 ${LEFT_COL} shrink-0`}>
                <Tooltip text={t('connection.window_title_tip')}>
                  <input
                    value={displayTargetTitle(selTitle, t)}
                    readOnly
                    placeholder={t('connection.window_title')}
                    className="flex-1 min-w-0 h-7 rounded-lg border border-border bg-bg-primary px-2 text-xs outline-none cursor-default text-text-muted truncate"
                  />
                </Tooltip>
                {onDisconnect && (
                  <Tooltip text={t('connection.disconnect_tip')}>
                    <button
                      onClick={() => {
                        onDisconnect()
                        setSelTitle(DESKTOP_TITLE)
                      }}
                      className="h-7 w-7 flex items-center justify-center rounded-md bg-accent-secondary/10 hover:bg-accent-secondary/20 text-accent-secondary border border-accent-secondary/20 transition-colors shrink-0"
                    >
                      <Unlink className="w-3.5 h-3.5" />
                    </button>
                  </Tooltip>
                )}
              </div>
              <ActionBtn
                icon={<MonitorUp className="w-3.5 h-3.5" />}
                label={t('connection.select')}
                title={t('connection.select_tip')}
                variant="primary"
                onClick={() => {
                  setPickerOpen(true)
                  addLog('[Window] opening picker')
                }}
              />
            </div>
            <div className="flex justify-between items-center gap-2">
              <div className={`flex items-center gap-1.5 ${LEFT_COL} shrink-0`}>
                <Tooltip text={t('connection.ip_tip')}>
                  <input
                    value={serverHost}
                    onChange={(e) => {
                      const v = e.target.value
                      if (v.includes('::')) {
                        const [a, b] = v.split('::', 2)
                        onServerHostChange(a.trim())
                        if (b?.trim()) onServerPortChange(b.trim())
                      } else onServerHostChange(v)
                    }}
                    placeholder={t('connection.ip_placeholder')}
                    disabled={serverConnected}
                    className="flex-1 min-w-0 h-7 rounded-lg border border-border bg-bg-primary px-2 text-xs text-text-primary outline-none focus:border-accent transition-colors placeholder:text-text-muted disabled:opacity-60"
                  />
                </Tooltip>
                <Tooltip text={t('connection.port_tip')}>
                  <input
                    value={serverPort}
                    onChange={(e) => {
                      const v = e.target.value
                      if (v.includes('::')) {
                        const [a, b] = v.split('::', 2)
                        onServerPortChange(a.trim())
                        if (b?.trim()) onServerHostChange(b.trim())
                      } else onServerPortChange(v)
                    }}
                    placeholder={t('connection.port_placeholder')}
                    disabled={serverConnected}
                    className="w-14 shrink-0 h-7 rounded-lg border border-border bg-bg-primary px-2 text-xs text-text-primary outline-none focus:border-accent transition-colors placeholder:text-text-muted disabled:opacity-60"
                  />
                </Tooltip>
              </div>
              <ActionBtn
                icon={serverConnected
                  ? <Unplug className="w-3.5 h-3.5" />
                  : <Cable className="w-3.5 h-3.5" />}
                label={serverConnected ? t('connection.disconnect_server') : t('connection.connect')}
                title={serverConnected ? t('connection.disconnect_server_tip') : t('connection.connect_tip')}
                variant={serverConnected ? 'outline-accent' : 'primary'}
                onClick={onToggleServer}
              />
            </div>
          </div>
        </div>
      </div>
      <TargetPickerModal
        open={pickerOpen}
        onClose={() => setPickerOpen(false)}
        onSelectWindow={handleSelectWindow}
        onSelectMode={handleSelectMode}
      />
    </div>
  )
}
