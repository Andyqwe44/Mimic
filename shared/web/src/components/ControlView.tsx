// ControlView — primary page for connection, peer, gates / preview.
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { ConnectionPanel } from './ConnectionPanel'
import { PeerPanel } from './PeerPanel'
import { PeerRemoteView } from './PeerRemoteView'
import { StreamGatesPanel } from './StreamGatesPanel'
import { ScreenshotPanel } from './ScreenshotPanel'
import { hostCall, addLog } from '../lib/bridge'
import { DESKTOP_TITLE, displayTargetTitle } from '../lib/windowTitle'
import { THIN_CLIENT } from '../lib/features'
import { RADIUS, RING, SHELL_PAD, TEXT } from '../lib/design'
import type { WindowInfo } from '../lib/types'

export function ControlView({
  selWin,
  setSelWin,
  winState,
  expectedCaptureState,
  setExpectedCaptureState,
  snapMethod,
  setSnapMethod,
  streamMethod,
  setStreamMethod,
  renderMethod,
  screenRatio,
  previewing,
  acceptControl,
  onTogglePreview,
  onToggleAcceptControl,
  takeSnapshot,
  previewingRef,
  snapshotRef,
  snapshotStartRef,
  capMethod,
  ssHasContentRef,
  onFps,
  onDims,
  serverConnected,
  peerControlMode,
  setPeerControlMode,
  peerRole,
  setPeerRole,
  peerTransport,
  setPeerTransport,
  remotePeerWindows,
  setRemotePeerWindows,
  linkReady,
}: {
  selWin: WindowInfo
  setSelWin: (w: WindowInfo) => void
  winState: string
  expectedCaptureState: string
  setExpectedCaptureState: (s: string) => void
  snapMethod: string
  setSnapMethod: (m: string) => void
  streamMethod: string
  setStreamMethod: (m: string) => void
  renderMethod: string
  screenRatio: number
  previewing: boolean
  acceptControl: boolean
  onTogglePreview: () => void
  onToggleAcceptControl: () => void
  takeSnapshot: () => void
  previewingRef: React.MutableRefObject<boolean>
  snapshotRef: React.MutableRefObject<boolean>
  snapshotStartRef: React.MutableRefObject<number>
  capMethod: string
  ssHasContentRef: React.MutableRefObject<boolean>
  onFps: (n: number) => void
  onDims: (w: number, h: number) => void
  serverConnected: boolean
  peerControlMode: 'human' | 'ai'
  setPeerControlMode: (m: 'human' | 'ai') => void
  peerRole: string
  setPeerRole: (r: string) => void
  peerTransport: string
  setPeerTransport: (m: string) => void
  remotePeerWindows: Array<{ title: string; hwnd: number; id?: string }>
  setRemotePeerWindows: (w: Array<{ title: string; hwnd: number; id?: string }>) => void
  linkReady: boolean
}) {
  const { t } = useTranslation()
  const [connExpanded, setConnExpanded] = useState(true)
  const [peerExpanded, setPeerExpanded] = useState(true)
  const [gatesExpanded, setGatesExpanded] = useState(true)
  const [ssExpanded, setSsExpanded] = useState(true)
  const [remoteTargetId, setRemoteTargetId] = useState('')

  const isController = peerRole === 'controller'
  const showLocalConnection = !isController

  const pickRemoteTarget = (w: { title: string; hwnd: number; id?: string }) => {
    const id = w.id || undefined
    setRemoteTargetId(id || String(w.hwnd))
    hostCall('peer_set_target', {
      hwnd: w.hwnd,
      title: w.title,
      id,
    })
      .then((res: { ok?: boolean; error?: string }) => {
        if (res?.ok === false) {
          addLog(`[Peer] set_target failed: ${res.error || 'unknown'}`)
          return
        }
        addLog(`[Peer] set_target ${w.title}${id ? ` (${id})` : ''}`)
      })
      .catch((e) => addLog(`[Peer] set_target failed: ${e}`))
  }

  return (
    <div className={`flex-1 overflow-y-auto ${SHELL_PAD.page} space-y-3 min-h-0`}>
      {/* Controller: peer first; local window picker is for controlled/idle only */}
      <PeerPanel
        expanded={peerExpanded}
        onToggle={() => setPeerExpanded((v) => !v)}
        controlMode={peerControlMode}
        onControlMode={setPeerControlMode}
        onRole={setPeerRole}
        onTransport={setPeerTransport}
        onRemoteWindows={(wins) => {
          setRemotePeerWindows(wins)
          addLog(`[Peer] remote windows: ${wins.length}`)
        }}
      />

      {isController && (
        <>
          <PeerRemoteView
            active={peerTransport !== 'none'}
            humanControl={peerControlMode === 'human'}
          />
          {peerControlMode === 'ai' && peerTransport !== 'none' && (
            <div className={`${TEXT.smallMono} text-amber-500 bg-warn-soft ${RADIUS.lg} px-2 py-1.5`}>
              {t('peer.ai_mode_hint')}
            </div>
          )}
          <div className={`${RADIUS.xl} bg-bg-secondary ${RING} p-2 space-y-1 max-h-56 overflow-y-auto min-w-0`}>
            <div className={`${TEXT.smallMono} font-medium text-text-secondary px-1 flex items-center justify-between gap-2`}>
              <span>{t('peer.remote_targets')}</span>
              {peerTransport !== 'none' && (
                <button
                  type="button"
                  className={`${TEXT.xs} text-accent shrink-0`}
                  onClick={() => {
                    hostCall('peer_request_windows')
                      .then(() => addLog('[Peer] refresh remote targets'))
                      .catch((e) => addLog(`[Peer] request_windows failed: ${e}`))
                  }}
                >
                  {t('peer.refresh_targets')}
                </button>
              )}
            </div>
            {peerTransport === 'none' && (
              <div className={`${TEXT.xs} text-text-muted px-1`}>{t('peer.wait_lan')}</div>
            )}
            {peerTransport !== 'none' && remotePeerWindows.length === 0 && (
              <div className={`${TEXT.xs} text-text-muted px-1`}>{t('peer.no_remote_targets')}</div>
            )}
            {remotePeerWindows.map((w) => {
              const key = w.id || String(w.hwnd)
              const selected = remoteTargetId === key
              return (
                <button
                  key={key}
                  type="button"
                  className={`w-full text-left ${TEXT.xs} px-2 py-2 min-h-11 ${RADIUS.md} truncate ${
                    selected ? 'bg-accent-soft text-accent' : 'hover:bg-bg-hover'
                  }`}
                  onClick={() => pickRemoteTarget(w)}
                >
                  {w.title || w.id || `(hwnd ${w.hwnd})`}
                </button>
              )
            })}
          </div>
        </>
      )}

      {showLocalConnection && (
        <ConnectionPanel
          onSelect={(w) => setSelWin(w)}
          onDisconnect={() => {
            setSelWin({ title: DESKTOP_TITLE, category: 'desktop', hwnd: 0 })
            setExpectedCaptureState('desktop')
            addLog('[Connection] disconnected, back to desktop')
          }}
          snapMethod={snapMethod}
          setSnapMethod={setSnapMethod}
          streamMethod={streamMethod}
          setStreamMethod={setStreamMethod}
          selWin={selWin}
          winState={winState}
          expectedCaptureState={expectedCaptureState}
          setExpectedCaptureState={setExpectedCaptureState}
          expanded={connExpanded}
          onToggle={() => setConnExpanded((v) => !v)}
        />
      )}

      {!isController && peerControlMode === 'ai' && peerTransport !== 'none' && (
        <div className={`${TEXT.smallMono} text-amber-500 bg-warn-soft ${RADIUS.lg} px-2 py-1.5`}>
          {t('peer.ai_mode_hint')}
        </div>
      )}

      {/* Gates / local preview are for controlled (or idle local use) — not the controlling device */}
      {!isController && (
        <div className="min-w-0">
          {THIN_CLIENT ? (
            <StreamGatesPanel
              streamOn={previewing}
              controlOn={acceptControl}
              onToggleStream={onTogglePreview}
              onToggleControl={onToggleAcceptControl}
              targetTitle={displayTargetTitle(selWin.title, t)}
              linkReady={linkReady || serverConnected || (peerRole === 'controlled' && peerTransport !== 'none')}
              expanded={gatesExpanded}
              onToggle={() => setGatesExpanded((v) => !v)}
            />
          ) : (
            <ScreenshotPanel
              selWin={selWin}
              screenRatio={screenRatio}
              snapMethod={snapMethod}
              streamMethod={streamMethod}
              renderMethod={renderMethod}
              winState={winState}
              expanded={ssExpanded}
              onToggle={() => setSsExpanded((v) => !v)}
              previewing={previewing}
              previewingRef={previewingRef}
              snapshotRef={snapshotRef}
              snapshotStartRef={snapshotStartRef}
              capMethod={capMethod}
              onTakeSnapshot={takeSnapshot}
              onTogglePreview={onTogglePreview}
              pinned={false}
              onTogglePin={() => {}}
              showPin={false}
              hasContentRef={ssHasContentRef}
              onFps={onFps}
              onDims={onDims}
            />
          )}
        </div>
      )}
    </div>
  )
}
