// ═══ Connection Panel — local capture target only (signaling lives in Peer) ═══
import { useState, useEffect } from 'react'
import { MonitorUp, Unlink } from 'lucide-react'
import { Tooltip, ActionBtn } from './Toolkit'
import { TargetPickerModal } from './TargetPickerModal'
import { RailCard } from './RailCard'
import { addLog } from '../lib/bridge'
import {
  METHOD_SHORT,
  STATE_LABEL,
  cantCaptureMinimized,
} from '../lib/constants'
import { DESKTOP_TITLE, displayTargetTitle, isDesktopTitle } from '../lib/windowTitle'
import { isAndroidHost } from '../lib/platform'
import { useTranslation } from 'react-i18next'
import type { WindowInfo } from '../lib/types'

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
}) {
  const { t } = useTranslation()
  const android = isAndroidHost()
  const [pickerOpen, setPickerOpen] = useState(false)
  const [selTitle, setSelTitle] = useState(DESKTOP_TITLE)
  const isDesktop = isDesktopTitle(selTitle) || selWin?.category === 'desktop' || selWin?.hwnd === 0
    || selWin?.kind === 'display' || selWin?.kind === 'desktop'

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
    if (selWin && selWin.title !== selTitle) setSelTitle(selWin.title)
  }, [selWin?.title])

  const cantCapture = !android && !isDesktop && cantCaptureMinimized(streamMethod, winState)
  const recommendedMethod = android
    ? 'mediaprojection'
    : winState === 'minimized' ? 'dxgi' : 'wgc'
  // Android badges parallel PC 「桌面 / WGC」: 「主屏|应用 / MP」
  const methodBadge = android
    ? (METHOD_SHORT.mediaprojection || 'MP')
    : (METHOD_SHORT[recommendedMethod] || recommendedMethod)
  const stateBadge = android
    ? (selWin?.kind === 'app' ? t('targetPicker.apps') : t('targetPicker.display'))
    : t(STATE_LABEL[winState] || winState)

  return (
    <>
      <RailCard
        icon={(
          <span className="w-5 h-5 rounded bg-blue-soft flex items-center justify-center text-blue-400">
            <MonitorUp className="w-3.5 h-3.5" strokeWidth={2} />
          </span>
        )}
        title={t('connection.title')}
        badges={[
          { text: stateBadge, tone: 'accent' },
          { text: methodBadge, tone: 'accent' },
        ]}
        expanded={expanded}
        onToggle={() => {
          onToggle()
          addLog(`[Connection] ${!expanded ? 'expanded' : 'collapsed'}`)
        }}
        pinned={pinned}
        onTogglePin={onTogglePin}
      >
        {cantCapture && (
          <div className="text-xs text-error bg-error-soft rounded-lg px-2 py-1.5">
            {t('connection.minimized_warning', { method: streamMethod.toUpperCase() })}
          </div>
        )}
        <div className="flex items-center gap-2 min-w-0">
          <div className="flex items-center gap-1.5 flex-1 min-w-0">
            <Tooltip text={t('connection.window_title_tip')}>
              <input
                value={displayTargetTitle(selTitle, t)}
                readOnly
                placeholder={t('connection.window_title')}
                className="w-full min-w-0 h-7 rounded-lg border border-border bg-bg-primary px-2 text-xs outline-none cursor-default text-text-muted truncate"
              />
            </Tooltip>
            {onDisconnect && (
              <Tooltip text={t('connection.disconnect_tip')}>
                <button
                  type="button"
                  onClick={() => {
                    onDisconnect()
                    setSelTitle(DESKTOP_TITLE)
                  }}
                  className="h-7 w-7 flex items-center justify-center rounded-md bg-accent-secondary-soft hover:bg-accent-secondary-soft-mid text-accent-secondary border border-accent-secondary-ring transition-colors shrink-0"
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
      </RailCard>
      <TargetPickerModal
        open={pickerOpen}
        onClose={() => setPickerOpen(false)}
        onSelectWindow={handleSelectWindow}
        onSelectMode={handleSelectMode}
      />
    </>
  )
}
