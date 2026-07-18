// SessionCapsule — always-visible connection / stream / control truth summary.
import { useTranslation } from 'react-i18next'
import { Tooltip } from './Toolkit'
import { TEXT, RADIUS } from '../lib/design'

export type CapsuleTone = 'muted' | 'info' | 'success' | 'danger'

export function SessionCapsule({
  device,
  connected,
  streaming,
  controlling,
  error,
  compact,
  onClick,
}: {
  device: string
  connected: boolean
  streaming: boolean
  controlling: boolean
  error?: boolean
  compact?: boolean
  onClick: () => void
}) {
  const { t } = useTranslation()

  let badge = t('capsule.disconnected')
  let tone: CapsuleTone = 'muted'
  if (error) {
    badge = t('capsule.error')
    tone = 'danger'
  } else if (controlling) {
    badge = t('capsule.control_on')
    tone = 'success'
  } else if (streaming) {
    badge = t('capsule.stream_on')
    tone = 'success'
  } else if (connected) {
    badge = t('capsule.connected')
    tone = 'info'
  }

  const toneCls =
    tone === 'success'
      ? 'text-success bg-success-soft'
      : tone === 'info'
        ? 'text-accent bg-accent-soft-mid'
        : tone === 'danger'
          ? 'text-error bg-error-soft'
          : 'text-text-muted bg-bg-tertiary'

  const tip = error
    ? t('capsule.tip_error')
    : t('capsule.tip_open_control')

  return (
    <Tooltip text={tip}>
      <button
        type="button"
        onClick={onClick}
        className={`inline-flex items-center gap-1.5 max-w-full min-h-9 px-2.5 py-1 ${RADIUS.full}
          bg-bg-secondary ring-1 ring-inset hover:bg-bg-hover transition-colors shrink-0
          ${error ? 'ring-error-ring' : streaming || controlling ? 'ring-accent-ring' : 'ring-border'}`}
      >
        <span className={`${TEXT.xs} font-semibold text-text-primary truncate max-w-[7rem]`}>
          {device}
        </span>
        <span className={`${TEXT.tiny} font-medium px-1.5 py-0.5 ${RADIUS.full} shrink-0 ${toneCls}`}>
          {badge}
        </span>
        {!compact && (
          <span className={`${TEXT.tiny} text-text-tertiary shrink-0 hidden min-[600px]:inline`}>
            {streaming ? t('capsule.stream_short_on') : t('capsule.stream_short_off')}
            {' · '}
            {controlling ? t('capsule.control_short_on') : t('capsule.control_short_off')}
          </span>
        )}
      </button>
    </Tooltip>
  )
}
