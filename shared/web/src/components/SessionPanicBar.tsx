// Controlled-side session emergency controls (pause stream / lock input / hangup).
import { Pause, Play, Lock, Unlock, PhoneOff } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Tooltip, ActionBtn } from './Toolkit'
import { RADIUS, RING, TEXT } from '../lib/design'

export function SessionPanicBar({
  streamOn,
  controlOn,
  onToggleStream,
  onToggleControl,
  onHangup,
}: {
  streamOn: boolean
  controlOn: boolean
  onToggleStream: () => void
  onToggleControl: () => void
  onHangup: () => void
}) {
  const { t } = useTranslation()

  return (
    <div className={`${RADIUS.xl} bg-bg-secondary ${RING} p-3 space-y-2 shrink-0`}>
      <div className={`${TEXT.smallMono} font-medium text-text-secondary`}>
        {t('session.panic_title')}
      </div>
      <p className={`${TEXT.xs} text-text-muted leading-relaxed`}>
        {t('session.panic_hint')}
      </p>
      <div className="flex flex-wrap gap-2">
        <Tooltip text={streamOn ? t('session.pause_stream_tip') : t('session.resume_stream_tip')}>
          <button
            type="button"
            onClick={onToggleStream}
            className={`inline-flex items-center gap-1.5 h-9 px-3 rounded-lg text-xs font-medium transition-colors ${
              streamOn
                ? 'bg-accent-soft-mid text-accent ring-1 ring-accent-ring'
                : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'
            }`}
          >
            {streamOn ? <Pause className="w-3.5 h-3.5" /> : <Play className="w-3.5 h-3.5" />}
            {streamOn ? t('session.pause_stream') : t('session.resume_stream')}
          </button>
        </Tooltip>
        <Tooltip text={controlOn ? t('session.lock_input_tip') : t('session.unlock_input_tip')}>
          <button
            type="button"
            onClick={onToggleControl}
            className={`inline-flex items-center gap-1.5 h-9 px-3 rounded-lg text-xs font-medium transition-colors ${
              controlOn
                ? 'bg-success-soft text-success ring-1 ring-success-ring'
                : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'
            }`}
          >
            {controlOn ? <Unlock className="w-3.5 h-3.5" /> : <Lock className="w-3.5 h-3.5" />}
            {controlOn ? t('session.lock_input') : t('session.unlock_input')}
          </button>
        </Tooltip>
        <ActionBtn
          icon={<PhoneOff className="w-3.5 h-3.5" />}
          label={t('peer.hangup')}
          title={t('peer.hangup_tip')}
          variant="danger"
          onClick={onHangup}
        />
      </div>
    </div>
  )
}
