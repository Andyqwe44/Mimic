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
  remotePeerWindows: Array<{ title: string; hwnd: number }>
  setRemotePeerWindows: (w: Array<{ title: string; hwnd: number }>) => void
  linkReady: boolean
}) {
  const { t } = useTranslation()
  const [connExpanded, setConnExpanded] = useState(true)
  const [peerExpanded, setPeerExpanded] = useState(true)
  const [gatesExpanded, setGatesExpanded] = useState(true)
  const [ssExpanded, setSsExpanded] = useState(true)

  return (
    <div className={`flex-1 overflow-y-auto ${SHELL_PAD.page} space-y-3 min-h-0`}>
      <div className="grid grid-cols-1 min-[960px]:grid-cols-2 gap-3">
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

        <div className="space-y-3 min-w-0">
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
          <PeerRemoteView
            active={peerRole === 'controller' && peerTransport !== 'none'}
            humanControl={peerControlMode === 'human'}
          />
          {peerControlMode === 'ai' && peerTransport !== 'none' && (
            <div className={`${TEXT.smallMono} text-amber-500 bg-amber-500/10 ${RADIUS.lg} px-2 py-1.5`}>
              {t('peer.ai_mode_hint')}
            </div>
          )}
          {remotePeerWindows.length > 0 && (
            <div className={`${RADIUS.xl} bg-bg-secondary ${RING} p-2 space-y-1 max-h-40 overflow-y-auto min-w-0`}>
              <div className={`${TEXT.smallMono} font-medium text-text-secondary px-1`}>
                {t('peer.remote_windows')}
              </div>
              {remotePeerWindows.map((w) => (
                <button
                  key={w.hwnd}
                  type="button"
                  className={`w-full text-left ${TEXT.xs} px-2 py-2 min-h-11 ${RADIUS.md} hover:bg-bg-hover truncate`}
                  onClick={() => {
                    hostCall('peer_set_target', { hwnd: w.hwnd, title: w.title })
                      .then(() => addLog(`[Peer] set_target ${w.title}`))
                      .catch((e) => addLog(`[Peer] set_target failed: ${e}`))
                  }}
                >
                  {w.title || `(hwnd ${w.hwnd})`}
                </button>
              ))}
            </div>
          )}
        </div>
      </div>

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
    </div>
  )
}
