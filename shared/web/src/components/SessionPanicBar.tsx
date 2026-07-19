// Controlled-side session emergency: refuse control (view-only) + hangup.
import { Lock, Unlock, PhoneOff } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { ActionBtn } from './Toolkit'
import { RADIUS, RING, TEXT, btnAutoSize } from '../lib/design'

export function SessionPanicBar({
  controlOn,
  onToggleControl,
  onHangup,
}: {
  controlOn: boolean
  onToggleControl: () => void
  onHangup: () => void
}) {
  const { t } = useTranslation()
  const controlLabel = controlOn ? t('session.refuse_control') : t('session.allow_control')
  const hangLabel = t('peer.hangup')
  // Same height (ActionBtn H.control) + same width tier so the pair stays aligned.
  const size = (() => {
    const a = btnAutoSize(controlLabel) as 'xs' | 'sm' | 'md' | 'lg' | 'xl'
    const b = btnAutoSize(hangLabel) as 'xs' | 'sm' | 'md' | 'lg' | 'xl'
    const order = ['xs', 'sm', 'md', 'lg', 'xl'] as const
    return order[Math.max(order.indexOf(a), order.indexOf(b))]
  })()

  return (
    <div className={`${RADIUS.xl} bg-bg-secondary ${RING} p-3 space-y-2 shrink-0`}>
      <div className={`${TEXT.smallMono} font-medium text-text-secondary`}>
        {t('session.panic_title')}
      </div>
      <p className={`${TEXT.xs} text-text-muted leading-relaxed`}>
        {t('session.panic_hint')}
      </p>
      <div className="flex flex-wrap gap-2">
        <ActionBtn
          icon={controlOn ? <Unlock className="w-3.5 h-3.5" /> : <Lock className="w-3.5 h-3.5" />}
          label={controlLabel}
          title={controlOn ? t('session.refuse_control_tip') : t('session.allow_control_tip')}
          variant={controlOn ? 'outline-accent' : 'outline'}
          size={size}
          onClick={onToggleControl}
        />
        <ActionBtn
          icon={<PhoneOff className="w-3.5 h-3.5" />}
          label={hangLabel}
          title={t('peer.hangup_tip')}
          variant="danger"
          size={size}
          onClick={onHangup}
        />
      </div>
    </div>
  )
}
