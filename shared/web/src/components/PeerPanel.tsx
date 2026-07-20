// ═══ Peer panel — signaling login + same-account devices (UU-style) ═══
import { useCallback, useEffect, useRef, useState } from 'react'
import { Cable, Monitor, PhoneOff, Bot, User, Radar } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { hostCall, addLog, onNativePush } from '../lib/bridge'
import { getHostPlatform } from '../lib/platform'
import { Tooltip, ActionBtn } from './Toolkit'
import { RailCard, type RailBadgeTone } from './RailCard'

type Device = {
  deviceId: string
  deviceName: string
  lanIps?: string[]
  online?: boolean
  /** Server presence: online | away (grace) — offline devices are omitted. */
  state?: 'online' | 'away' | 'offline'
  platform?: string
  peerProto?: number
}
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
  onSessionStart,
  onSessionEnd,
}: {
  expanded: boolean
  onToggle: () => void
  pinned?: boolean
  onTogglePin?: () => void
  onRemoteWindows?: (wins: Array<{ title: string; hwnd: number; id?: string }>) => void
  onTransport?: (mode: string) => void
  onRole?: (role: string) => void
  controlMode: 'human' | 'ai'
  onControlMode: (m: 'human' | 'ai') => void
  /** Fired when a peer session becomes active (navigate to Monitor). */
  onSessionStart?: () => void
  /** Call ended — navigate back to Peers (hangup / peer_disconnect). */
  onSessionEnd?: (reason?: string) => void
}) {
  const { t } = useTranslation()
  // Default = public Mimic signaling (Aliyun). Override anytime in the field.
  const defaultDeviceName = () => {
    const plat = getHostPlatform()
    const prefix = plat === 'android' ? 'Android' : plat === 'windows' ? 'PC' : 'Web'
    const hint = typeof navigator !== 'undefined'
      ? (navigator.platform || navigator.userAgent || 'dev').slice(0, 10).replace(/\s+/g, '')
      : 'dev'
    return `${prefix}-${hint}`
  }
  const [url, setUrl] = useState('http://47.107.43.5:8443')
  const [user, setUser] = useState('demo')
  const [password, setPassword] = useState('demo')
  const [deviceName, setDeviceName] = useState(defaultDeviceName)
  const [credsReady, setCredsReady] = useState(false)
  const [online, setOnline] = useState(false)
  const [reconnecting, setReconnecting] = useState(false)
  const [role, setRole] = useState('idle')
  const [devices, setDevices] = useState<Device[]>([])
  const [myId, setMyId] = useState('')
  const [transport, setTransport] = useState('none')
  const [status, setStatus] = useState('')
  const [probe, setProbe] = useState<ProbeState>('idle')
  const [rttMs, setRttMs] = useState<number | null>(null)
  const rttMsRef = useRef<number | null>(null)
  rttMsRef.current = rttMs

  const [clusterN, setClusterN] = useState(0)

  const pollRef = useRef(0)
  const probeRef = useRef(0)
  const saveCredsRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  // Stable push handler — avoid re-subscribe churn duplicating addLog.
  const pushHandlerRef = useRef<(d: Record<string, unknown>) => void>(() => {})
  // Single-slot frame take: at most one in-flight peer_get_frame; coalesce notifies.
  const frameBusyRef = useRef(false)
  const framePendingRef = useRef(false)
  const frameEmptyLogRef = useRef(0)

  const pullPeerFrame = useCallback(() => {
    if (frameBusyRef.current) {
      framePendingRef.current = true
      return
    }
    frameBusyRef.current = true
    framePendingRef.current = false
    hostCall('peer_get_frame').then((fr: {
      ok?: boolean; w?: number; h?: number; flags?: number; b64?: string; error?: string
    }) => {
      if (!fr?.ok || !fr.b64) {
        // Rate-limit empty-slot logs (structural coalesce should make these rare).
        const now = Date.now()
        if (fr?.error && fr.error !== 'no frame' && now - frameEmptyLogRef.current > 2000) {
          frameEmptyLogRef.current = now
          addLog(`[Decode] peer_get_frame: ${fr.error}`)
        }
      } else {
        const bin = Uint8Array.from(atob(fr.b64), (c) => c.charCodeAt(0))
        window.dispatchEvent(new CustomEvent('peer-h264', {
          detail: { ...fr, bytes: bin, source: 'remote' },
        }))
      }
    }).catch((e) => addLog(`[Decode] peer_get_frame failed: ${e}`))
      .finally(() => {
        frameBusyRef.current = false
        if (framePendingRef.current) pullPeerFrame()
      })
  }, [])

  // PC WebView2: H.264 via SharedBuffer (zero base64) — preferred over peer_get_frame.
  useEffect(() => {
    const wv = window.chrome?.webview
    if (!wv?.addEventListener) return
    const onSb = (e: {
      getBuffer: () => ArrayBuffer
      additionalData: unknown
    }) => {
      try {
        const metaRaw = e.additionalData
        const meta = (typeof metaRaw === 'string' ? JSON.parse(metaRaw) : metaRaw) as {
          kind?: string; w?: number; h?: number; flags?: number; ts?: number
        }
        if (meta?.kind !== 'h264') return
        const buf = e.getBuffer()
        if (!buf || buf.byteLength < 16) return
        const bytes = new Uint8Array(buf.slice(0))
        window.dispatchEvent(new CustomEvent('peer-h264', {
          detail: { w: meta.w, h: meta.h, flags: meta.flags, bytes, source: 'remote' },
        }))
      } catch { /* ignore non-h264 / parse errors */ }
    }
    wv.addEventListener('sharedbufferreceived', onSb)
    return () => wv.removeEventListener('sharedbufferreceived', onSb)
  }, [])

  // Load persisted signaling credentials (local prefs only; wire still uses passHash).
  useEffect(() => {
    hostCall('get_settings').then((res: { settings?: Record<string, unknown> }) => {
      const s = res?.settings
      if (s && typeof s === 'object') {
        if (typeof s.peerUrl === 'string' && s.peerUrl) setUrl(s.peerUrl)
        if (typeof s.peerUser === 'string' && s.peerUser) setUser(s.peerUser)
        if (typeof s.peerPassword === 'string') setPassword(s.peerPassword)
        if (typeof s.peerDeviceName === 'string' && s.peerDeviceName) setDeviceName(s.peerDeviceName)
      }
    }).catch(() => {}).finally(() => setCredsReady(true))
  }, [])

  // Persist credentials (debounced).
  useEffect(() => {
    if (!credsReady) return
    if (saveCredsRef.current) clearTimeout(saveCredsRef.current)
    saveCredsRef.current = setTimeout(() => {
      hostCall('get_settings').then((res: { settings?: Record<string, unknown> }) => {
        const prev = (res?.settings && typeof res.settings === 'object') ? res.settings : {}
        return hostCall('set_settings', {
          settings: {
            ...prev,
            peerUrl: url,
            peerUser: user,
            peerPassword: password,
            peerDeviceName: deviceName,
          },
        })
      }).catch(() => {})
    }, 800)
    return () => { if (saveCredsRef.current) clearTimeout(saveCredsRef.current) }
  }, [credsReady, url, user, password, deviceName])

  const refreshStatus = useCallback(async (opts?: { pullDevices?: boolean }) => {
    try {
      const st = await hostCall('peer_status')
      // Keep device-list UI while logged in / reconnecting (socket may be down).
      const stillIn = !!(st?.logged_in || st?.online || st?.reconnecting)
      setOnline(stillIn)
      setReconnecting(!!st?.reconnecting && !st?.online)
      const r = st?.role || 'idle'
      setRole(r)
      onRole?.(r)
      if (st?.deviceId) setMyId(st.deviceId)
      // Always sync transport (incl. "none") — mediaReady can heal a missed peer_transport push.
      const tx = typeof st?.transport === 'string' ? st.transport
        : (st?.mediaReady ? 'lan' : 'none')
      setTransport(tx)
      onTransport?.(tx)
      // Device roster is server-pushed on join/leave/meta change.
      // Explicit pull only on login/reconnect recovery (not on a timer).
      if (opts?.pullDevices && st?.online) {
        hostCall('peer_list_devices').catch(() => {})
      }
    } catch { /* */ }
  }, [onTransport, onRole])

  const probeServer = useCallback(async (silent = false) => {
    const base = url.trim().replace(/\/$/, '')
    if (!base) {
      setProbe('missing')
      setRttMs(null)
      setClusterN(0)
      if (!silent) setStatus(t('peer.server_missing'))
      return
    }
    setProbe('probing')
    try {
      // Same stack as login: C++ WinHTTP GET /health + /api/cluster (铁律 5).
      const res = await hostCall('peer_probe', { url: base }) as {
        ok?: boolean; rtt_ms?: number; node_count?: number; error?: string
      }
      if (!res?.ok) {
        setProbe('missing')
        setRttMs(null)
        setClusterN(0)
        if (!silent) setStatus(res?.error === 'unreachable' ? t('peer.server_missing') : (res?.error || t('peer.probe_fail')))
        return
      }
      const ms = typeof res.rtt_ms === 'number' ? res.rtt_ms : null
      const n = typeof res.node_count === 'number' && res.node_count > 0 ? res.node_count : 1
      setProbe('ok')
      setRttMs(ms)
      setClusterN(n)
      window.dispatchEvent(new CustomEvent('peer-link-stats', {
        detail: { rtt_ms: ms },
      }))
      if (!silent) setStatus(ms != null ? t('peer.probe_ok', { ms }) : t('peer.probe_ok', { ms: '?' }))
    } catch (e) {
      setProbe('missing')
      setRttMs(null)
      setClusterN(0)
      if (!silent) setStatus(String(e))
    }
  }, [url, t])

  useEffect(() => {
    pushHandlerRef.current = (d: Record<string, unknown>) => {
      if (!d || typeof d !== 'object') return
      if (d.type === 'devices' && Array.isArray(d.devices)) {
        setDevices(d.devices as Device[])
        addLog(`[Peer] devices update: ${(d.devices as Device[]).length}`)
      } else if (d.type === 'invite') {
        // Banner owns incoming UX + log; only sync role here (avoid ×2 invite logs).
        setRole('ringing')
      } else if (d.type === 'invite_sent') {
        setRole('outgoing')
      } else if (d.type === 'invite_rejected') {
        setRole('idle')
        setStatus(t('peer.invite_rejected'))
      } else if (d.type === 'session_start') {
        setStatus(t('peer.session_started'))
        // Role must be synced before Monitor workspace switches layout.
        void refreshStatus().then(() => onSessionStart?.())
      } else if (d.type === 'session_end') {
        const reason = String(d.reason || '')
        setRole('idle')
        onRole?.('idle')
        setTransport('none')
        setStatus(
          reason === 'peer_disconnect'
            ? t('peer.peer_device_offline')
            : t('peer.session_ended'),
        )
        onTransport?.('none')
        window.dispatchEvent(new CustomEvent('peer-link-stats', {
          detail: { transport: 'none', rtt_ms: rttMsRef.current },
        }))
        onRemoteWindows?.([])
        onSessionEnd?.(reason)
      } else if (d.type === 'peer_transport') {
        const mode = String(d.mode || 'none')
        setTransport(mode)
        onTransport?.(mode)
        window.dispatchEvent(new CustomEvent('peer-link-stats', {
          detail: { transport: mode, rtt_ms: rttMsRef.current },
        }))
        // LAN ready → controller pulls remote window/desktop list
        if (mode !== 'none') {
          hostCall('peer_request_windows').catch(() => {})
        }
      } else if (d.type === 'error') {
        const code = String(d.code || '')
        const err = String(d.error || 'error')
        setStatus(code === 'busy' ? t('peer.busy') : err)
        addLog(`[Peer] ${code ? `${code}: ` : ''}${err}`)
      } else if (d.type === 'peer_frame') {
        pullPeerFrame()
      } else if (d.type === 'peer_msg') {
        const payload = d.payload as {
          type?: string
          windows?: Array<{ title?: string; hwnd?: number; id?: string }>
          targets?: Array<{ title?: string; hwnd?: number; id?: string }>
        } | undefined
        if (payload?.type === 'list_windows' || payload?.type === 'list_targets') {
          const wins = payload.targets?.length ? payload.targets : (payload.windows || [])
          onRemoteWindows?.(wins.map((w) => ({
            title: w.title || '',
            hwnd: w.hwnd || 0,
            id: w.id,
          })))
        }
      } else if (d.type === 'peer_error') {
        setStatus(String(d.error || 'peer error'))
        addLog(`[Peer] ${d.error}`)
      } else if (d.type === 'peer_offline') {
        setOnline(false)
        setReconnecting(false)
        setRole('idle')
        setStatus(t('peer.offline'))
        addLog('[Peer] offline')
      } else if (d.type === 'peer_reconnecting') {
        // Keep logged-in UI; signaling is retrying in background.
        setOnline(true)
        setReconnecting(true)
        setStatus(t('peer.reconnecting'))
        addLog(`[Peer] reconnecting: ${d.reason || ''}`)
      } else if (d.type === 'peer_online') {
        setOnline(true)
        setReconnecting(false)
        setStatus(t('peer.online'))
        if (d.reconnected) addLog('[Peer] reconnected')
        // Catch-up once after (re)connect — roster is then event-pushed.
        void refreshStatus({ pullDevices: true })
      }
    }
  }, [onRemoteWindows, onTransport, onRole, onSessionStart, onSessionEnd, refreshStatus, t, pullPeerFrame])

  useEffect(() => {
    return onNativePush((d: Record<string, unknown>) => {
      pushHandlerRef.current(d)
    })
  }, [])

  useEffect(() => {
    // Light status poll (role / transport). Faster while in session so LAN state heals quickly.
    const ms = role !== 'idle' ? 2000 : 15000
    pollRef.current = window.setInterval(() => { void refreshStatus() }, ms) as unknown as number
    void refreshStatus({ pullDevices: true })
    return () => clearInterval(pollRef.current)
  }, [refreshStatus, role])

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
      setReconnecting(false)
      setMyId(res.deviceId || '')
      setStatus(t('peer.online'))
      addLog(`[Peer] logged in as ${user}`)
      void refreshStatus({ pullDevices: true })
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
    setReconnecting(false)
    setDevices([])
    setRole('idle')
    onRole?.('idle')
    onRemoteWindows?.([])
    setStatus(t('peer.logged_out'))
    probeServer(true)
  }

  const invite = async (id: string) => {
    const res = await hostCall('peer_invite', { targetDeviceId: id })
    if (res?.ok === false) setStatus(res.error || t('peer.invite_failed'))
    else setStatus(t('peer.invite_sent'))
  }

  const hangup = async () => {
    try { await hostCall('set_stream_gate', { enabled: false }) } catch { /* */ }
    try { await hostCall('set_control_gate', { enabled: false }) } catch { /* */ }
    await hostCall('peer_hangup')
    setRole('idle')
    onRole?.('idle')
    setTransport('none')
    onTransport?.('none')
    setStatus(t('peer.session_ended'))
    onSessionEnd?.('hangup')
  }

  const others = devices.filter((d) => d.deviceId !== myId)

  const headerBadges: Array<{ text: string; tone?: RailBadgeTone }> = []
  if (online) {
    if (reconnecting) {
      headerBadges.push({ text: t('peer.reconnecting'), tone: 'warn' })
    } else if (role === 'controller') headerBadges.push({ text: t('peer.role_controller'), tone: 'accent' })
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
  } else if (probe === 'ok') {
    headerBadges.push({
      text: rttMs != null
        ? t('peer.badge_reachable', { ms: rttMs })
        : t('peer.badge_reachable_plain'),
      tone: 'success',
    })
  } else {
    headerBadges.push({ text: t('peer.badge_offline'), tone: 'muted' })
  }

  return (
    <RailCard
      icon={(
        <span className="w-5 h-5 rounded bg-accent-soft flex items-center justify-center text-accent">
          <Cable className="w-3.5 h-3.5" strokeWidth={2} />
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
          <div className="text-[10px] text-text-muted bg-bg-tertiary rounded-lg px-2 py-1.5">
            {t('peer.cluster_hint', { n: probe === 'ok' ? clusterN : 0 })}
          </div>
        </>
      ) : (
        <>
          <div className="flex items-center justify-between gap-2 min-w-0">
            <span className="text-[11px] text-text-tertiary truncate min-w-0">{user} · {deviceName}</span>
            <button type="button" className="text-[11px] text-accent shrink-0" onClick={logout}>{t('peer.logout')}</button>
          </div>

          {/* Incoming accept/reject lives in IncomingCallBanner (single UX). */}

          {(role === 'controller' || role === 'controlled') && (
            <div className="flex flex-wrap gap-2 items-center min-w-0">
              <ActionBtn icon={<PhoneOff className="w-3.5 h-3.5" />} label={t('peer.hangup')} title={t('peer.hangup_tip')}
                variant="danger" onClick={hangup} />
              {role === 'controller' && transport !== 'none' && onSessionStart && (
                <ActionBtn
                  icon={<Monitor className="w-3.5 h-3.5" />}
                  label={t('peer.enter_monitor')}
                  title={t('peer.enter_monitor_tip')}
                  variant="primary"
                  onClick={() => onSessionStart()}
                />
              )}
              {role === 'controller' && (
                <div className="flex gap-1 ml-auto items-center">
                  <span className="text-[10px] text-text-muted hidden sm:inline">{t('peer.control_mode')}</span>
                  <Tooltip text={t('peer.human_tip')}>
                    <button type="button" onClick={() => {
                      onControlMode('human')
                      hostCall('peer_set_control_mode', { mode: 'human' })
                    }}
                      className={`h-7 px-2 rounded-md inline-flex items-center gap-1 text-[11px] ${controlMode === 'human' ? 'bg-accent-soft-mid text-accent' : 'text-text-muted hover:bg-bg-hover'}`}>
                      <User className="w-3.5 h-3.5" />
                      <span>{t('peer.human_short')}</span>
                    </button>
                  </Tooltip>
                  <Tooltip text={t('peer.ai_tip')}>
                    <button type="button" onClick={() => {
                      onControlMode('ai')
                      hostCall('peer_set_control_mode', { mode: 'ai' })
                    }}
                      className={`h-7 px-2 rounded-md inline-flex items-center gap-1 text-[11px] ${controlMode === 'ai' ? 'bg-accent-soft-mid text-accent' : 'text-text-muted hover:bg-bg-hover'}`}>
                      <Bot className="w-3.5 h-3.5" />
                      <span>{t('peer.ai_short')}</span>
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
            {others.map((d) => {
              const away = d.state === 'away' || d.online === false
              const canInvite = role === 'idle' && !away
              return (
              <div key={d.deviceId} className={`flex items-center gap-2 rounded-lg border border-border px-2 py-1.5 min-w-0 ${away ? 'opacity-60' : ''}`}>
                <Monitor className="w-3.5 h-3.5 text-text-muted shrink-0" />
                <div className="flex-1 min-w-0">
                  <div className="text-xs text-text-primary truncate">{d.deviceName}</div>
                  <div className="text-[10px] text-text-tertiary truncate">
                    {away ? `${t('peer.device_away')} · ` : ''}
                    {d.platform ? `${d.platform} · ` : ''}{d.deviceId}
                    {d.peerProto ? ` · v${d.peerProto}` : ''}
                  </div>
                </div>
                {canInvite && (
                  <button type="button"
                    className="text-[11px] px-2 py-1 rounded-md bg-accent text-white shrink-0"
                    onClick={() => invite(d.deviceId)}>
                    {t('peer.request_control')}
                  </button>
                )}
              </div>
              )
            })}
          </div>
        </>
      )}
      {status && <div className="text-[10px] text-text-muted break-words">{status}</div>}
    </RailCard>
  )
}
