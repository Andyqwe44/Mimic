// App-level incoming-call banner — Android top bubble / PC top-right slide-in.
// Auto-dismiss + peer_reject after INCOMING_TIMEOUT_MS if user does not act.
import { useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Phone, PhoneOff } from 'lucide-react'
import { hostCall, addLog, onNativePush } from '../lib/bridge'
import { isAndroidHost } from '../lib/platform'
import { ActionBtn } from './Toolkit'
import { TEXT, RADIUS } from '../lib/design'

const INCOMING_TIMEOUT_MS = 45_000

type Incoming = { fromDeviceId: string; fromDeviceName: string }

export function IncomingCallBanner({
  onAccepted,
}: {
  /** Optional — jump to Monitor after accept (same as PeerPanel). */
  onAccepted?: () => void
}) {
  const { t } = useTranslation()
  const android = isAndroidHost()
  const [incoming, setIncoming] = useState<Incoming | null>(null)
  const [visible, setVisible] = useState(false)
  const timerRef = useRef<number | null>(null)
  const incomingRef = useRef<Incoming | null>(null)
  const onAcceptedRef = useRef(onAccepted)
  onAcceptedRef.current = onAccepted
  incomingRef.current = incoming

  const clearTimer = () => {
    if (timerRef.current != null) {
      window.clearTimeout(timerRef.current)
      timerRef.current = null
    }
  }

  const dismiss = () => {
    clearTimer()
    setVisible(false)
    window.setTimeout(() => setIncoming(null), 280)
  }

  const accept = async () => {
    const cur = incomingRef.current
    if (!cur) return
    dismiss()
    try {
      await hostCall('peer_accept', { fromDeviceId: cur.fromDeviceId })
      addLog(`[Peer] accept (banner) ${cur.fromDeviceName}`)
      onAcceptedRef.current?.()
    } catch (e) {
      addLog(`[Peer] accept failed: ${e}`)
    }
  }

  const reject = async (reason: 'user' | 'timeout') => {
    const cur = incomingRef.current
    if (!cur) return
    dismiss()
    try {
      await hostCall('peer_reject', { fromDeviceId: cur.fromDeviceId })
      addLog(`[Peer] reject (banner/${reason}) ${cur.fromDeviceName}`)
    } catch (e) {
      addLog(`[Peer] reject failed: ${e}`)
    }
  }

  useEffect(() => {
    return onNativePush((d: Record<string, unknown>) => {
      if (!d || typeof d !== 'object') return
      if (d.type === 'invite') {
        const next: Incoming = {
          fromDeviceId: String(d.fromDeviceId || ''),
          fromDeviceName: String(d.fromDeviceName || d.fromDeviceId || ''),
        }
        if (!next.fromDeviceId) return
        clearTimer()
        setIncoming(next)
        requestAnimationFrame(() => setVisible(true))
        timerRef.current = window.setTimeout(() => {
          void (async () => {
            const cur = incomingRef.current
            if (!cur) return
            clearTimer()
            setVisible(false)
            window.setTimeout(() => setIncoming(null), 280)
            try {
              await hostCall('peer_reject', { fromDeviceId: cur.fromDeviceId })
              addLog(`[Peer] reject (banner/timeout) ${cur.fromDeviceName}`)
            } catch (e) {
              addLog(`[Peer] reject failed: ${e}`)
            }
          })()
        }, INCOMING_TIMEOUT_MS)
        addLog(`[Peer] incoming banner: ${next.fromDeviceName}`)
      } else if (
        d.type === 'invite_rejected' ||
        d.type === 'session_start' ||
        d.type === 'session_end' ||
        d.type === 'peer_offline'
      ) {
        clearTimer()
        setVisible(false)
        window.setTimeout(() => setIncoming(null), 280)
      }
    })
  }, [])

  useEffect(() => () => clearTimer(), [])

  if (!incoming) return null

  const title = android
    ? t('peer.call_banner_android', { name: incoming.fromDeviceName })
    : t('peer.call_banner_pc', { name: incoming.fromDeviceName })

  if (android) {
    return (
      <div
        className="fixed inset-x-0 z-[200] flex justify-center pointer-events-none"
        style={{ top: 'max(8px, env(safe-area-inset-top, 0px))' }}
        data-no-page-swipe
      >
        <div
          className={`pointer-events-auto mx-3 max-w-md w-full ${RADIUS.xl} bg-bg-secondary/95 backdrop-blur-md ring-1 ring-inset ring-border shadow-lg px-3 py-2.5 transition-transform duration-300 ease-out ${
            visible ? 'translate-y-0 opacity-100' : '-translate-y-[120%] opacity-0'
          }`}
        >
          <div className={`${TEXT.xs} text-text-primary font-medium truncate`}>{title}</div>
          <div className="grid grid-cols-2 gap-2 mt-2">
            <ActionBtn
              icon={<Phone className="w-3.5 h-3.5" />}
              label={t('peer.accept')}
              title={t('peer.accept_tip')}
              variant="primary"
              size="fill"
              onClick={() => { void accept() }}
            />
            <ActionBtn
              icon={<PhoneOff className="w-3.5 h-3.5" />}
              label={t('peer.reject')}
              title={t('peer.reject_tip')}
              variant="outline"
              size="fill"
              onClick={() => { void reject('user') }}
            />
          </div>
        </div>
      </div>
    )
  }

  return (
    <div
      className="fixed z-[200] pointer-events-none"
      style={{
        top: 'max(12px, env(safe-area-inset-top, 0px))',
        right: 'max(12px, env(safe-area-inset-right, 0px))',
      }}
      data-no-page-swipe
    >
      <div
        className={`pointer-events-auto w-[min(320px,92vw)] ${RADIUS.xl} bg-bg-secondary/95 backdrop-blur-md ring-1 ring-inset ring-border shadow-lg px-3 py-2.5 transition-transform duration-300 ease-out ${
          visible ? 'translate-x-0 opacity-100' : 'translate-x-[110%] opacity-0'
        }`}
      >
        <div className={`${TEXT.xs} text-text-primary font-medium truncate`}>{title}</div>
        <div className="flex gap-2 mt-2">
          <ActionBtn
            icon={<Phone className="w-3.5 h-3.5" />}
            label={t('peer.accept')}
            title={t('peer.accept_tip')}
            variant="primary"
            size="sm"
            onClick={() => { void accept() }}
          />
          <ActionBtn
            icon={<PhoneOff className="w-3.5 h-3.5" />}
            label={t('peer.reject')}
            title={t('peer.reject_tip')}
            variant="outline"
            size="sm"
            onClick={() => { void reject('user') }}
          />
        </div>
      </div>
    </div>
  )
}
