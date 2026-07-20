// Peers page — signaling login, devices, invite/accept/hangup (no remote video).
import { useState } from 'react'
import { PeerPanel } from './PeerPanel'
import { ShizukuConnectCard } from './ShizukuConnectCard'
import { SHELL_PAD } from '../lib/design'
import { isAndroidHost } from '../lib/platform'

export function ControlView({
  peerControlMode,
  setPeerControlMode,
  setPeerRole,
  setPeerTransport,
  setRemotePeerWindows,
  onSessionStart,
  onSessionEnd,
}: {
  peerControlMode: 'human' | 'ai'
  setPeerControlMode: (m: 'human' | 'ai') => void
  setPeerRole: (r: string) => void
  setPeerTransport: (m: string) => void
  setRemotePeerWindows: (w: Array<{ title: string; hwnd: number; id?: string }>) => void
  onSessionStart?: () => void
  onSessionEnd?: (reason?: string) => void
}) {
  const [peerExpanded, setPeerExpanded] = useState(true)
  const [shizukuExpanded, setShizukuExpanded] = useState(true)

  return (
    <div className={`flex-1 overflow-y-auto ${SHELL_PAD.page} space-y-3 min-h-0`}>
      {isAndroidHost() && (
        <ShizukuConnectCard
          expanded={shizukuExpanded}
          onToggle={() => setShizukuExpanded((v) => !v)}
        />
      )}
      <PeerPanel
        expanded={peerExpanded}
        onToggle={() => setPeerExpanded((v) => !v)}
        controlMode={peerControlMode}
        onControlMode={setPeerControlMode}
        onRole={setPeerRole}
        onTransport={setPeerTransport}
        onRemoteWindows={(wins) => {
          setRemotePeerWindows(wins)
        }}
        onSessionStart={onSessionStart}
        onSessionEnd={onSessionEnd}
      />
    </div>
  )
}
