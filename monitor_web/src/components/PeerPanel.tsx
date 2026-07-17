// ═══ Peer panel — signaling login + same-account devices (UU-style) ═══
import { useCallback, useEffect, useRef, useState } from 'react'
import { Cable, Monitor, Phone, PhoneOff, Bot, User, Radar } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { hostCall, addLog } from '../lib/bridge'
import { Tooltip, ActionBtn } from './Toolkit'
import { RailCard, type RailBadgeTone } from './RailCard'

type Device = { deviceId: string; deviceName: string; lanIps?: string[]; online?: boolean }
type ProbeState = 'idle' | 'probing' | 'ok' | 'missing'

const inputCls =
  'w-full min-w-0 h-7 rounded-lg border border-border bg-bg-primary px-2 text-xs text-text-primary outline-none focus:border-accent transition-colors placeholder:text-text-muted'

export function PeerPanel({
  expanded,
  onToggle,
  pinned,
  onTogglePin,
  onRemoteWindows,
  onTransport,
  onRole,
  controlMode,
  onControlMode,
}: {
  expanded: boolean
  onToggle: () => void
  pinned?: boolean
  onTogglePin?: () => void
  onRemoteWindows?: (wins: Array<{ title: string; hwnd: number }>) => void
  onTransport?: (mode: string) => void
  onRole?: (role: string) => void
  controlMode: 'human' | 'ai'
  onControlMode: (m: 'human' | 'ai') => void
}) {
  const { t } = useTranslation()
  const [url, setUrl] = useState('http://127.0.0.1:8443')
  const [user, setUser] = useState('demo')
  const [password, setPassword] = useState('demo')
  const [deviceName, setDeviceName] = useState(
    () => 'PC-' + (typeof navigator !== 'undefined' ? navigator.platform.slice(0, 8) : 'win'),
  )
  const [online, setOnline] = useState(false)
  const [role, setRole] = useState('idle')
  const [devices, setDevices] = useState<Device[]>([])
  const [myId, setMyId] = useState('')
  const [transport, setTransport] = useState('none')
  const [incoming, setIncoming] = useState<{ fromDeviceId: string; fromDeviceName: string } | null>(null)
  const [status, setStatus] = useState('')
  const [probe, setProbe] = useState<ProbeState>('idle')
  const [rttMs, setRttMs] = useState<number | null>(null)

  const pollRef = useRef(0)
  const probeRef = useRef(0)

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

  const probeServer = useCallback(async (silent = false) => {
    const base = url.trim().replace(/\/$/, '')
    if (!base) {
      setProbe('missing')
      setRttMs(null)
      if (!silent) setStatus(t('peer.server_missing'))
      return
    }
    setProbe('probing')
    try {
      // Same stack as login: C++ WinHTTP GET /health (not WebView fetch — 铁律 5).
      const res = await hostCall('peer_probe', { url: base }) as {
        ok?: boolean; rtt_ms?: number; error?: string
      }
      if (!res?.ok) {
        setProbe('missing')
        setRttMs(null)
        if (!silent) setStatus(res?.error === 'unreachable' ? t('peer.server_missing') : (res?.error || t('peer.probe_fail')))
        return
      }
      const ms = typeof res.rtt_ms === 'number' ? res.rtt_ms : null
      setProbe('ok')
      setRttMs(ms)
      if (!silent) setStatus(ms != null ? t('peer.probe_ok', { ms }) : t('peer.probe_ok', { ms: '?' }))
    } catch (e) {
      setProbe('missing')
      setRttMs(null)
      if (!silent) setStatus(String(e))
    }
  }, [url, t])

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
        setStatus(t('peer.invite_rejected'))
      } else if (d.type === 'session_start') {
        setIncoming(null)
        setStatus(t('peer.session_started'))
        refreshStatus()
        hostCall('peer_request_windows').catch(() => {})
      } else if (d.type === 'session_end') {
        setRole('idle')
        onRole?.('idle')
        setTransport('none')
        setStatus(t('peer.session_ended'))
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
  }, [onRemoteWindows, onTransport, onRole, refreshStatus, t])

  useEffect(() => {
    pollRef.current = window.setInterval(() => { refreshStatus() }, 3000) as unknown as number
    refreshStatus()
    return () => clearInterval(pollRef.current)
  }, [refreshStatus])

  // Auto-probe when URL changes / panel opens (not logged in)
  useEffect(() => {
    if (online) return
    window.clearTimeout(probeRef.current)
    probeRef.current = window.setTimeout(() => { probeServer(true) }, 400) as unknown as number
    return () => window.clearTimeout(probeRef.current)
  }, [url, online, probeServer])

  const login = async () => {
    try {
      await probeServer(true)
      const res = await hostCall('peer_login', {
        url: url.trim(),
        user: user.trim(),
        password,
        deviceName,
      })
      if (res?.ok === false) {
        setStatus(res.error || t('peer.login_failed'))
        return
      }
      setOnline(true)
      setMyId(res.deviceId || '')
      setStatus(t('peer.online'))
      addLog(`[Peer] logged in as ${user}`)
    } catch (e) {
      setStatus(String(e))
    }
  }

  const register = async () => {
    try {
      const res = await hostCall('peer_register', { url: url.trim(), user: user.trim(), password })
      setStatus(res?.ok ? t('peer.register_ok') : (res?.error || t('peer.register_failed')))
    } catch (e) {
      setStatus(String(e))
    }
  }

  const logout = async () => {
    await hostCall('peer_logout')
    setOnline(false)
    setDevices([])
    setRole('idle')
    onRole?.('idle')
    setStatus(t('peer.logged_out'))
    probeServer(true)
  }

  const invite = async (id: string) => {
    const res = await hostCall('peer_invite', { targetDeviceId: id })
    if (res?.ok === false) setStatus(res.error || t('peer.invite_failed'))
    else setStatus(t('peer.invite_sent'))
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

  const headerBadges: Array<{ text: string; tone?: RailBadgeTone }> = []
  if (online) {
    if (role === 'controller') headerBadges.push({ text: t('peer.role_controller'), tone: 'accent' })
    else if (role === 'controlled') headerBadges.push({ text: t('peer.role_controlled'), tone: 'accent' })
    else if (role === 'outgoing') headerBadges.push({ text: t('peer.role_outgoing'), tone: 'warn' })
    else if (role === 'ringing') headerBadges.push({ text: t('peer.role_ringing'), tone: 'warn' })
    else headerBadges.push({
      text: rttMs != null ? t('peer.badge_online_rtt', { ms: rttMs }) : t('peer.badge_online'),
      tone: 'success',
    })
    if (transport !== 'none') {
      headerBadges.push({ text: transport.toUpperCase(), tone: 'accent' })
    }
  } else if (probe === 'probing') {
    headerBadges.push({ text: t('peer.badge_probing'), tone: 'warn' })
  } else if (probe === 'missing') {
    headerBadges.push({ text: t('peer.badge_missing'), tone: 'error' })
  } else if (probe === 'ok' && rttMs != null) {
    headerBadges.push({ text: t('peer.badge_reachable', { ms: rttMs }), tone: 'success' })
  } else {
    headerBadges.push({ text: t('peer.badge_offline'), tone: 'muted' })
  }

  return (
    <RailCard
      icon={(
        <span className="w-5 h-5 rounded bg-accent/15 flex items-center justify-center">
          <Cable className="w-3 h-3 text-accent" />
        </span>
      )}
      title={t('peer.title')}
      badges={headerBadges}
      expanded={expanded}
      onToggle={onToggle}
      pinned={pinned}
      onTogglePin={onTogglePin}
      maxBodyClass="max-h-[420px]"
    >
      {!online ? (
        <>
          <label className="block space-y-1 min-w-0">
            <span className="text-[11px] text-text-secondary">{t('peer.server_url')}</span>
            <input
              className={inputCls}
              value={url}
              onChange={(e) => setUrl(e.target.value)}
              placeholder={t('peer.server_url_ph')}
            />
          </label>
          <div className="grid grid-cols-2 gap-1.5 min-w-0">
            <label className="block space-y-1 min-w-0">
              <span className="text-[11px] text-text-secondary">{t('peer.user')}</span>
              <input className={inputCls} value={user} onChange={(e) => setUser(e.target.value)} placeholder={t('peer.user_ph')} />
            </label>
            <label className="block space-y-1 min-w-0">
              <span className="text-[11px] text-text-secondary">{t('peer.password')}</span>
              <input className={inputCls} type="password" value={password} onChange={(e) => setPassword(e.target.value)} placeholder={t('peer.password_ph')} />
            </label>
          </div>
          <label className="block space-y-1 min-w-0">
            <span className="text-[11px] text-text-secondary">{t('peer.device_name')}</span>
            <input className={inputCls} value={deviceName} onChange={(e) => setDeviceName(e.target.value)} placeholder={t('peer.device_name_ph')} />
          </label>
          <div className="flex flex-wrap gap-2 min-w-0">
            <ActionBtn icon={<Cable className="w-3.5 h-3.5" />} label={t('peer.login')} title={t('peer.login_tip')}
              variant="primary" onClick={login} />
            <ActionBtn icon={<User className="w-3.5 h-3.5" />} label={t('peer.register')} title={t('peer.register_tip')}
              variant="outline" onClick={register} />
            <ActionBtn icon={<Radar className="w-3.5 h-3.5" />} label={t('peer.probe')} title={t('peer.probe_tip')}
              variant="outline" onClick={() => probeServer(false)} />
          </div>
          {/* Cluster placeholder */}
          <div className="text-[10px] text-text-muted bg-bg-tertiary/60 rounded-lg px-2 py-1.5">
            {t('peer.cluster_hint', { n: probe === 'ok' ? 1 : 0 })}
          </div>
        </>
      ) : (
        <>
          <div className="flex items-center justify-between gap-2 min-w-0">
            <span className="text-[11px] text-text-tertiary truncate min-w-0">{user} · {deviceName}</span>
            <button type="button" className="text-[11px] text-accent shrink-0" onClick={logout}>{t('peer.logout')}</button>
          </div>

          {incoming && (
            <div className="rounded-lg bg-amber-500/10 p-2 space-y-2 min-w-0">
              <div className="text-xs text-text-primary truncate">{t('peer.invite_from', { name: incoming.fromDeviceName })}</div>
              <div className="flex flex-wrap gap-2">
                <ActionBtn icon={<Phone className="w-3.5 h-3.5" />} label={t('peer.accept')} title={t('peer.accept_tip')}
                  variant="primary" onClick={accept} />
                <ActionBtn icon={<PhoneOff className="w-3.5 h-3.5" />} label={t('peer.reject')} title={t('peer.reject_tip')}
                  variant="outline" onClick={reject} />
              </div>
            </div>
          )}

          {(role === 'controller' || role === 'controlled') && (
            <div className="flex flex-wrap gap-2 items-center min-w-0">
              <ActionBtn icon={<PhoneOff className="w-3.5 h-3.5" />} label={t('peer.hangup')} title={t('peer.hangup_tip')}
                variant="danger" onClick={hangup} />
              {role === 'controller' && (
                <div className="flex gap-1 ml-auto">
                  <Tooltip text={t('peer.human_tip')}>
                    <button type="button" onClick={() => {
                      onControlMode('human')
                      hostCall('peer_set_control_mode', { mode: 'human' })
                    }}
                      className={`h-7 w-7 rounded-md flex items-center justify-center ${controlMode === 'human' ? 'bg-accent/20 text-accent' : 'text-text-muted hover:bg-bg-hover'}`}>
                      <User className="w-3.5 h-3.5" />
                    </button>
                  </Tooltip>
                  <Tooltip text={t('peer.ai_tip')}>
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

          <div className="text-[11px] font-medium text-text-secondary pt-0.5">
            {t('peer.devices')} ({others.length})
          </div>
          <div className="space-y-1 min-w-0">
            {others.length === 0 && (
              <div className="text-[11px] text-text-muted">{t('peer.no_devices')}</div>
            )}
            {others.map((d) => (
              <div key={d.deviceId} className="flex items-center gap-2 rounded-lg border border-border px-2 py-1.5 min-w-0">
                <Monitor className="w-3.5 h-3.5 text-text-muted shrink-0" />
                <div className="flex-1 min-w-0">
                  <div className="text-xs text-text-primary truncate">{d.deviceName}</div>
                  <div className="text-[10px] text-text-tertiary truncate">{d.deviceId}</div>
                </div>
                {role === 'idle' && (
                  <button type="button"
                    className="text-[11px] px-2 py-1 rounded-md bg-accent text-white shrink-0"
                    onClick={() => invite(d.deviceId)}>
                    {t('peer.control')}
                  </button>
                )}
              </div>
            ))}
          </div>
        </>
      )}
      {status && <div className="text-[10px] text-text-muted break-words">{status}</div>}
    </RailCard>
  )
}
