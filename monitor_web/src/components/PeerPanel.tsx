// ═══ Peer panel — login, device list, invite, control mode (UU-style) ═══
import { useCallback, useEffect, useRef, useState } from 'react'
import { Cable, Monitor, Phone, PhoneOff, Bot, User } from 'lucide-react'
import { hostCall, addLog } from '../lib/bridge'
import { Tooltip, ActionBtn } from './Toolkit'
import { COLLAPSIBLE_HEADER } from '../lib/constants'

type Device = { deviceId: string; deviceName: string; lanIps?: string[]; online?: boolean }

export function PeerPanel({
  expanded,
  onToggle,
  onRemoteWindows,
  onTransport,
  onRole,
  controlMode,
  onControlMode,
}: {
  expanded: boolean
  onToggle: () => void
  onRemoteWindows?: (wins: Array<{ title: string; hwnd: number }>) => void
  onTransport?: (mode: string) => void
  onRole?: (role: string) => void
  controlMode: 'human' | 'ai'
  onControlMode: (m: 'human' | 'ai') => void
}) {
  const [url, setUrl] = useState('http://127.0.0.1:8443')
  const [user, setUser] = useState('demo')
  const [password, setPassword] = useState('demo')
  const [deviceName, setDeviceName] = useState(() => 'PC-' + (typeof navigator !== 'undefined' ? navigator.platform.slice(0, 8) : 'win'))
  const [online, setOnline] = useState(false)
  const [role, setRole] = useState('idle')
  const [devices, setDevices] = useState<Device[]>([])
  const [myId, setMyId] = useState('')
  const [transport, setTransport] = useState('none')
  const [incoming, setIncoming] = useState<{ fromDeviceId: string; fromDeviceName: string } | null>(null)
  const [status, setStatus] = useState('')

  const pollRef = useRef(0)

  const refreshStatus = useCallback(async () => {
    try {
      const st = await hostCall('peer_status')
      setOnline(!!st?.online)
      const r = st?.role || 'idle'
      setRole(r)
      onRole?.(r)
      if (st?.deviceId) setMyId(st.deviceId)
      if (st?.transport) {
        setTransport(st.transport)
        onTransport?.(st.transport)
      }
    } catch { /* */ }
  }, [onTransport, onRole])

  useEffect(() => {
  const onMsg = (e: { data: unknown }) => {
      const d = e.data as Record<string, unknown> | null
      if (!d || typeof d !== 'object') return
      if (d.type === 'devices' && Array.isArray(d.devices)) {
        setDevices(d.devices as Device[])
      } else if (d.type === 'invite') {
        setIncoming({
          fromDeviceId: String(d.fromDeviceId || ''),
          fromDeviceName: String(d.fromDeviceName || d.fromDeviceId || ''),
        })
        setRole('ringing')
        addLog(`[Peer] invite from ${d.fromDeviceName || d.fromDeviceId}`)
      } else if (d.type === 'invite_sent') {
        setRole('outgoing')
      } else if (d.type === 'invite_rejected') {
        setIncoming(null)
        setRole('idle')
        setStatus('Invite rejected')
      } else if (d.type === 'session_start') {
        setIncoming(null)
        setStatus('Session started')
        refreshStatus()
        hostCall('peer_request_windows').catch(() => {})
      } else if (d.type === 'session_end') {
        setRole('idle')
        onRole?.('idle')
        setTransport('none')
        setStatus('Session ended')
        onTransport?.('none')
      } else if (d.type === 'peer_transport') {
        setTransport(String(d.mode || 'none'))
        onTransport?.(String(d.mode || 'none'))
      } else if (d.type === 'peer_frame') {
        hostCall('peer_get_frame').then((fr: {
          ok?: boolean; w?: number; h?: number; flags?: number; b64?: string
        }) => {
          if (!fr?.ok || !fr.b64) return
          const bin = Uint8Array.from(atob(fr.b64), (c) => c.charCodeAt(0))
          window.dispatchEvent(new CustomEvent('peer-h264', { detail: { ...fr, bytes: bin } }))
        }).catch(() => {})
      } else if (d.type === 'peer_msg') {
        const payload = d.payload as { type?: string; windows?: Array<{ title?: string; hwnd?: number }> } | undefined
        if (payload?.type === 'list_windows') {
          const wins = payload.windows || []
          onRemoteWindows?.(wins.map((w) => ({
            title: w.title || '',
            hwnd: w.hwnd || 0,
          })))
        }
      } else if (d.type === 'peer_error') {
        setStatus(String(d.error || 'peer error'))
        addLog(`[Peer] ${d.error}`)
      } else if (d.type === 'peer_offline') {
        setOnline(false)
        setRole('idle')
      }
    }
    window.chrome?.webview?.addEventListener('message', onMsg as (e: { data: unknown }) => void)
    return () => window.chrome?.webview?.removeEventListener('message', onMsg as (e: { data: unknown }) => void)
  }, [onRemoteWindows, onTransport, onRole, refreshStatus])

  useEffect(() => {
    pollRef.current = window.setInterval(() => { refreshStatus() }, 3000) as unknown as number
    refreshStatus()
    return () => clearInterval(pollRef.current)
  }, [refreshStatus])

  const login = async () => {
    try {
      const res = await hostCall('peer_login', {
        url: url.trim(),
        user: user.trim(),
        password,
        deviceName,
      })
      if (res?.ok === false) {
        setStatus(res.error || 'login failed')
        return
      }
      setOnline(true)
      setMyId(res.deviceId || '')
      setStatus('Online')
      addLog(`[Peer] logged in as ${user}`)
    } catch (e) {
      setStatus(String(e))
    }
  }

  const register = async () => {
    try {
      const res = await hostCall('peer_register', { url: url.trim(), user: user.trim(), password })
      setStatus(res?.ok ? 'Registered — now login' : (res?.error || 'register failed'))
    } catch (e) {
      setStatus(String(e))
    }
  }

  const logout = async () => {
    await hostCall('peer_logout')
    setOnline(false)
    setDevices([])
    setRole('idle')
    setStatus('Logged out')
  }

  const invite = async (id: string) => {
    const res = await hostCall('peer_invite', { targetDeviceId: id })
    if (res?.ok === false) setStatus(res.error || 'invite failed')
    else setStatus('Invite sent…')
  }

  const accept = async () => {
    if (!incoming) return
    await hostCall('peer_accept', { fromDeviceId: incoming.fromDeviceId })
    setIncoming(null)
  }

  const reject = async () => {
    if (!incoming) return
    await hostCall('peer_reject', { fromDeviceId: incoming.fromDeviceId })
    setIncoming(null)
    setRole('idle')
  }

  const hangup = async () => {
    await hostCall('peer_hangup')
    setRole('idle')
    onRole?.('idle')
    setTransport('none')
    onTransport?.('none')
  }

  const others = devices.filter((d) => d.deviceId !== myId)

  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
      <div role="button" tabIndex={0} onClick={onToggle} className={COLLAPSIBLE_HEADER}
        onKeyDown={(e) => { if (e.key === 'Enter' || e.key === ' ') (e.currentTarget as HTMLElement).click() }}>
        <div className="flex items-center gap-2 min-w-0">
          <Cable className="w-3.5 h-3.5 text-accent shrink-0" />
          <span className="text-sm font-medium text-text-primary">Peer</span>
          <span className={`text-[11px] px-1.5 py-0.5 rounded ${online ? 'bg-emerald-500/15 text-emerald-500' : 'bg-bg-tertiary text-text-muted'}`}>
            {online ? role : 'offline'}
          </span>
          {transport !== 'none' && (
            <span className="text-[11px] px-1.5 py-0.5 rounded bg-accent/15 text-accent uppercase">{transport}</span>
          )}
        </div>
      </div>
      {expanded && (
        <div className="border-t border-border p-3 space-y-2 max-h-[420px] overflow-y-auto">
          {!online ? (
            <>
              <input className="w-full h-7 rounded-lg border border-border bg-bg-primary px-2 text-xs"
                value={url} onChange={(e) => setUrl(e.target.value)} placeholder="http://signaling:8443" />
              <div className="flex gap-1.5">
                <input className="flex-1 h-7 rounded-lg border border-border bg-bg-primary px-2 text-xs"
                  value={user} onChange={(e) => setUser(e.target.value)} placeholder="user" />
                <input className="flex-1 h-7 rounded-lg border border-border bg-bg-primary px-2 text-xs"
                  type="password" value={password} onChange={(e) => setPassword(e.target.value)} placeholder="password" />
              </div>
              <input className="w-full h-7 rounded-lg border border-border bg-bg-primary px-2 text-xs"
                value={deviceName} onChange={(e) => setDeviceName(e.target.value)} placeholder="device name" />
              <div className="flex gap-2">
                <ActionBtn icon={<Cable className="w-3.5 h-3.5" />} label="Login" title="Login to signaling"
                  variant="primary" onClick={login} />
                <ActionBtn icon={<User className="w-3.5 h-3.5" />} label="Register" title="Create account"
                  variant="outline" onClick={register} />
              </div>
            </>
          ) : (
            <>
              <div className="flex items-center justify-between gap-2">
                <span className="text-[11px] text-text-tertiary truncate">{user} · {deviceName}</span>
                <button type="button" className="text-[11px] text-accent" onClick={logout}>Logout</button>
              </div>

              {incoming && (
                <div className="rounded-lg bg-amber-500/10 p-2 space-y-2">
                  <div className="text-xs text-text-primary">Invite from {incoming.fromDeviceName}</div>
                  <div className="flex gap-2">
                    <ActionBtn icon={<Phone className="w-3.5 h-3.5" />} label="Accept" title="Allow control"
                      variant="primary" onClick={accept} />
                    <ActionBtn icon={<PhoneOff className="w-3.5 h-3.5" />} label="Reject" title="Reject"
                      variant="outline" onClick={reject} />
                  </div>
                </div>
              )}

              {(role === 'controller' || role === 'controlled') && (
                <div className="flex gap-2 items-center">
                  <ActionBtn icon={<PhoneOff className="w-3.5 h-3.5" />} label="Hang up" title="End session"
                    variant="danger" onClick={hangup} />
                  {role === 'controller' && (
                    <div className="flex gap-1 ml-auto">
                      <Tooltip text="Human control">
                        <button type="button" onClick={() => {
                          onControlMode('human')
                          hostCall('peer_set_control_mode', { mode: 'human' })
                        }}
                          className={`h-7 w-7 rounded-md flex items-center justify-center ${controlMode === 'human' ? 'bg-accent/20 text-accent' : 'text-text-muted hover:bg-bg-hover'}`}>
                          <User className="w-3.5 h-3.5" />
                        </button>
                      </Tooltip>
                      <Tooltip text="AI control (local model)">
                        <button type="button" onClick={() => {
                          onControlMode('ai')
                          hostCall('peer_set_control_mode', { mode: 'ai' })
                        }}
                          className={`h-7 w-7 rounded-md flex items-center justify-center ${controlMode === 'ai' ? 'bg-accent/20 text-accent' : 'text-text-muted hover:bg-bg-hover'}`}>
                          <Bot className="w-3.5 h-3.5" />
                        </button>
                      </Tooltip>
                    </div>
                  )}
                </div>
              )}

              <div className="text-[11px] font-medium text-text-secondary pt-1">Devices</div>
              <div className="space-y-1">
                {others.length === 0 && (
                  <div className="text-[11px] text-text-muted">No other devices online</div>
                )}
                {others.map((d) => (
                  <div key={d.deviceId} className="flex items-center gap-2 rounded-lg border border-border px-2 py-1.5">
                    <Monitor className="w-3.5 h-3.5 text-text-muted shrink-0" />
                    <div className="flex-1 min-w-0">
                      <div className="text-xs text-text-primary truncate">{d.deviceName}</div>
                      <div className="text-[10px] text-text-tertiary truncate">{d.deviceId}</div>
                    </div>
                    {role === 'idle' && (
                      <button type="button"
                        className="text-[11px] px-2 py-1 rounded-md bg-accent text-white"
                        onClick={() => invite(d.deviceId)}>
                        Control
                      </button>
                    )}
                  </div>
                ))}
              </div>
            </>
          )}
          {status && <div className="text-[10px] text-text-muted">{status}</div>}
        </div>
      )}
    </div>
  )
}
