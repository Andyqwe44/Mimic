// Android Shizuku / privilege gate — Peers page (MAA-Meow-style readiness card).
import { useCallback, useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Shield, RefreshCw, ExternalLink, BookOpen } from 'lucide-react'
import { hostCall, addLog, onNativePush } from '../lib/bridge'
import { isAndroidHost } from '../lib/platform'
import { ActionBtn, Tooltip } from './Toolkit'
import { RailCard, type RailBadgeTone } from './RailCard'
import { TEXT, RADIUS, GAP } from '../lib/design'
import { ShizukuGuideModal } from './ShizukuGuideModal'

type CapStatus = {
  ok?: boolean
  backend?: string
  preferred?: string
  restoring?: boolean
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
  const [guideOpen, setGuideOpen] = useState(false)

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
    const off = onNativePush((msg: { type?: string; status?: CapStatus }) => {
      if (msg?.type === 'capability_status' && msg.status) {
        setSt(msg.status)
      }
    })
    return () => {
      clearInterval(id)
      off()
    }
  }, [refresh])

  if (!isAndroidHost()) return null

  const sh = st?.shizuku
  const backend = st?.backend || 'normal'
  const preferred = st?.preferred || 'normal'
  const shState = (sh?.state || 'unavailable').toLowerCase()
  const connected = backend === 'shizuku' && (sh?.granted || shState === 'connected')
  const restoring = !!st?.restoring || shState === 'connecting'

  let badgeTone: RailBadgeTone = 'muted'
  let badgeText = t('peer.shizuku_badge_off')
  if (connected) {
    badgeTone = 'success'
    badgeText = t('peer.shizuku_badge_on')
  } else if (restoring || (preferred === 'shizuku' && !connected)) {
    badgeTone = 'warn'
    badgeText = restoring
      ? t('peer.shizuku_badge_restoring')
      : (shState === 'available' || sh?.available
        ? t('peer.shizuku_badge_need_auth')
        : t('peer.shizuku_badge_off'))
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
  const primaryConnected = connected
  const busyCls = busy ? 'opacity-50 pointer-events-none' : undefined

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
      headerActions={(
        <Tooltip text={t('peer.shizuku_refresh_tip')}>
          <button
            type="button"
            className="p-1 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors"
            onClick={(e) => {
              e.stopPropagation()
              void refresh()
            }}
          >
            <RefreshCw className="w-3.5 h-3.5" />
          </button>
        </Tooltip>
      )}
    >
      <p className={`${TEXT.xs} text-text-muted leading-relaxed`}>
        {t('peer.shizuku_body')}
      </p>
      <div className={`${TEXT.tiny} text-text-tertiary ${RADIUS.md} bg-bg-tertiary px-2 py-1.5 space-y-0.5`}>
        <div>{t('peer.shizuku_backend', { backend })}</div>
        <div>{t('peer.shizuku_state', { state: shState })}</div>
        {detail ? <div className="truncate">{detail}</div> : null}
      </div>
      {/* Three compact actions on one row — tips keep full wording. */}
      <div className={`grid grid-cols-3 ${GAP.md}`}>
        {primaryConnected ? (
          <ActionBtn
            icon={<Shield className="w-3.5 h-3.5" />}
            label={t('peer.shizuku_use_normal_short')}
            title={t('peer.shizuku_use_normal_tip')}
            variant="outline"
            size="fill"
            onClick={() => { if (!busy) void useNormal() }}
            className={busyCls}
          />
        ) : (
          <ActionBtn
            icon={<Shield className="w-3.5 h-3.5" />}
            label={busy ? t('peer.shizuku_connecting_short') : t('peer.shizuku_connect_short')}
            title={t('peer.shizuku_connect_tip')}
            variant="primary"
            size="fill"
            onClick={() => { if (!busy) void connect() }}
            className={busyCls}
          />
        )}
        <ActionBtn
          icon={<ExternalLink className="w-3.5 h-3.5" />}
          label={t('peer.shizuku_open_app_short')}
          title={t('peer.shizuku_open_app_tip')}
          variant="outline"
          size="fill"
          onClick={openApp}
        />
        <ActionBtn
          icon={<BookOpen className="w-3.5 h-3.5" />}
          label={t('peer.shizuku_guide_btn_short')}
          title={t('peer.shizuku_guide_btn_tip')}
          variant="outline"
          size="fill"
          onClick={() => setGuideOpen(true)}
        />
      </div>
      {msg && <div className={`${TEXT.tiny} text-text-muted break-words`}>{msg}</div>}
      <ShizukuGuideModal
        open={guideOpen}
        onClose={() => setGuideOpen(false)}
        onOpenApp={openApp}
      />
    </RailCard>
  )
}
