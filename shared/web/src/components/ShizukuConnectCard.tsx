// Android Shizuku / privilege gate — Peers page (MAA-Meow-style readiness card).
import { useCallback, useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Shield, RefreshCw, ExternalLink } from 'lucide-react'
import { hostCall, addLog } from '../lib/bridge'
import { isAndroidHost } from '../lib/platform'
import { ActionBtn, Tooltip } from './Toolkit'
import { RailCard, type RailBadgeTone } from './RailCard'
import { TEXT, RADIUS } from '../lib/design'

type CapStatus = {
  ok?: boolean
  backend?: string
  available?: string[]
  shizuku?: { available?: boolean; granted?: boolean; state?: string; detail?: string }
  root?: { available?: boolean; granted?: boolean; state?: string; detail?: string }
  normal?: { available?: boolean; granted?: boolean; state?: string; detail?: string }
}

export function ShizukuConnectCard({
  expanded,
  onToggle,
}: {
  expanded: boolean
  onToggle: () => void
}) {
  const { t } = useTranslation()
  const [st, setSt] = useState<CapStatus | null>(null)
  const [busy, setBusy] = useState(false)
  const [msg, setMsg] = useState('')

  const refresh = useCallback(async () => {
    try {
      const res = await hostCall('get_capability_backend') as CapStatus
      setSt(res)
      return res
    } catch (e) {
      setMsg(String(e))
      return null
    }
  }, [])

  useEffect(() => {
    if (!isAndroidHost()) return
    void refresh()
    const id = window.setInterval(() => { void refresh() }, 4000)
    return () => clearInterval(id)
  }, [refresh])

  if (!isAndroidHost()) return null

  const sh = st?.shizuku
  const backend = st?.backend || 'normal'
  const shState = (sh?.state || 'unavailable').toLowerCase()
  const connected = backend === 'shizuku' && (sh?.granted || shState === 'connected')

  let badgeTone: RailBadgeTone = 'muted'
  let badgeText = t('peer.shizuku_badge_off')
  if (connected) {
    badgeTone = 'success'
    badgeText = t('peer.shizuku_badge_on')
  } else if (shState === 'available' || sh?.available) {
    badgeTone = 'warn'
    badgeText = t('peer.shizuku_badge_need_auth')
  } else if (shState === 'unavailable') {
    badgeTone = 'error'
    badgeText = t('peer.shizuku_badge_missing')
  }

  const connect = async () => {
    setBusy(true)
    setMsg('')
    try {
      const res = await hostCall('set_capability_backend', { backend: 'shizuku' })
      if (res?.ok === false) {
        setMsg(res.error || t('peer.shizuku_connect_failed'))
        addLog(`[Shizuku] connect refused: ${res.error || 'error'}`)
      } else {
        setMsg(t('peer.shizuku_connected'))
        addLog('[Shizuku] backend = shizuku')
      }
      await refresh()
    } catch (e) {
      setMsg(String(e))
      addLog(`[Shizuku] connect failed: ${e}`)
    } finally {
      setBusy(false)
    }
  }

  const useNormal = async () => {
    setBusy(true)
    try {
      await hostCall('set_capability_backend', { backend: 'normal' })
      addLog('[Shizuku] backend = normal')
      await refresh()
      setMsg(t('peer.shizuku_using_normal'))
    } catch (e) {
      setMsg(String(e))
    } finally {
      setBusy(false)
    }
  }

  const openApp = async () => {
    try {
      const res = await hostCall('open_shizuku')
      if (res?.ok === false) {
        setMsg(res.error || t('peer.shizuku_open_failed'))
        addLog(`[Shizuku] open failed: ${res.error}`)
      }
    } catch (e) {
      setMsg(String(e))
    }
  }

  const detail = sh?.detail || ''

  return (
    <RailCard
      icon={(
        <span className="w-5 h-5 rounded bg-accent-soft flex items-center justify-center text-accent">
          <Shield className="w-3.5 h-3.5" strokeWidth={2} />
        </span>
      )}
      title={t('peer.shizuku_title')}
      badges={[{ text: badgeText, tone: badgeTone }]}
      expanded={expanded}
      onToggle={onToggle}
      maxBodyClass="max-h-[360px]"
    >
      <p className={`${TEXT.xs} text-text-muted leading-relaxed`}>
        {t('peer.shizuku_body')}
      </p>
      <div className={`${TEXT.tiny} text-text-tertiary ${RADIUS.md} bg-bg-tertiary px-2 py-1.5 space-y-0.5`}>
        <div>{t('peer.shizuku_backend', { backend })}</div>
        <div>{t('peer.shizuku_state', { state: shState })}</div>
        {detail ? <div className="truncate">{detail}</div> : null}
      </div>
      <div className="flex flex-wrap gap-2">
        <ActionBtn
          icon={<Shield className="w-3.5 h-3.5" />}
          label={busy ? t('peer.shizuku_connecting') : t('peer.shizuku_connect')}
          title={t('peer.shizuku_connect_tip')}
          variant="primary"
          onClick={() => { if (!busy && !connected) void connect() }}
          className={busy || connected ? 'opacity-50 pointer-events-none' : undefined}
        />
        <ActionBtn
          icon={<ExternalLink className="w-3.5 h-3.5" />}
          label={t('peer.shizuku_open_app')}
          title={t('peer.shizuku_open_app_tip')}
          variant="outline"
          onClick={openApp}
        />
        <Tooltip text={t('peer.shizuku_refresh_tip')}>
          <button
            type="button"
            className="h-7 w-7 rounded-md flex items-center justify-center text-text-muted hover:bg-bg-hover"
            onClick={() => { void refresh() }}
          >
            <RefreshCw className="w-3.5 h-3.5" />
          </button>
        </Tooltip>
        {backend === 'shizuku' && (
          <ActionBtn
            icon={<Shield className="w-3.5 h-3.5" />}
            label={t('peer.shizuku_use_normal')}
            title={t('peer.shizuku_use_normal_tip')}
            variant="outline"
            onClick={() => { if (!busy) void useNormal() }}
            className={busy ? 'opacity-50 pointer-events-none' : undefined}
          />
        )}
      </div>
      {msg && <div className={`${TEXT.tiny} text-text-muted break-words`}>{msg}</div>}
    </RailCard>
  )
}
