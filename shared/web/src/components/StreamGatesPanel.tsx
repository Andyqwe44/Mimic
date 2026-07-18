// ═══ Stream / Control gates — thin-client right-rail card ═══
import { Play, Square, Power } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Tooltip } from './Toolkit'
import { RailCard } from './RailCard'

export function StreamGatesPanel({
  streamOn,
  controlOn,
  onToggleStream,
  onToggleControl,
  targetTitle,
  linkReady = false,
  expanded,
  onToggle,
  pinned,
  onTogglePin,
}: {
  streamOn: boolean
  controlOn: boolean
  onToggleStream: () => void
  onToggleControl: () => void
  targetTitle: string
  /** Peer controlled session or legacy controller_server link */
  linkReady?: boolean
  expanded: boolean
  onToggle: () => void
  pinned?: boolean
  onTogglePin?: () => void
}) {
  const { t } = useTranslation()
  const streamDisabled = !linkReady && !streamOn

  const badges = [
    {
      text: streamOn ? t('gates.badge_stream_on') : t('gates.badge_stream_off'),
      tone: streamOn ? 'accent' as const : 'muted' as const,
    },
    {
      text: controlOn ? t('gates.badge_control_on') : t('gates.badge_control_off'),
      tone: controlOn ? 'success' as const : 'muted' as const,
    },
  ]

  return (
    <RailCard
      icon={(
        <span className="w-5 h-5 rounded bg-success-soft flex items-center justify-center text-emerald-500">
          <Power className="w-3.5 h-3.5" strokeWidth={2} />
        </span>
      )}
      title={t('gates.title')}
      badges={badges}
      expanded={expanded}
      onToggle={onToggle}
      pinned={pinned}
      onTogglePin={onTogglePin}
      maxBodyClass="max-h-[320px]"
    >
      <div className="text-[11px] text-text-tertiary truncate">
        {t('gates.target')}: {targetTitle}
      </div>

      {!linkReady && (
        <div className="text-[11px] text-amber-500 bg-warn-soft rounded-lg px-2 py-1.5">
          {t('gates.need_link')}
        </div>
      )}

      <div className="flex flex-col gap-2 min-w-0">
        <Tooltip text={streamDisabled ? t('gates.need_link') : t('monitor.stream_gate_tip')}>
          <button
            type="button"
            disabled={streamDisabled}
            onClick={onToggleStream}
            className={`w-full flex items-center gap-2 px-3 py-2.5 rounded-lg text-sm font-medium transition-colors disabled:opacity-50 disabled:cursor-not-allowed min-w-0 ${
              streamOn
                ? 'bg-accent-soft-mid text-accent ring-1 ring-accent-ring'
                : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'
            }`}
          >
            {streamOn ? <Square className="w-4 h-4 shrink-0" /> : <Play className="w-4 h-4 shrink-0" />}
            <span className="flex-1 text-left truncate">
              {streamOn ? t('monitor.stream_gate_on') : t('monitor.stream_gate')}
            </span>
            <span className={`text-[10px] px-1.5 py-0.5 rounded shrink-0 ${
              streamOn ? 'bg-accent-soft-mid text-accent' : 'bg-bg-hover text-text-muted'
            }`}>
              {streamOn ? t('monitor.gate_open') : t('monitor.gate_closed')}
            </span>
          </button>
        </Tooltip>

        <Tooltip text={t('monitor.control_gate_tip')}>
          <button
            type="button"
            onClick={onToggleControl}
            className={`w-full flex items-center gap-2 px-3 py-2.5 rounded-lg text-sm font-medium transition-colors min-w-0 ${
              controlOn
                ? 'bg-success-soft text-success ring-1 ring-success-ring'
                : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'
            }`}
          >
            <Power className={`w-4 h-4 shrink-0 ${controlOn ? 'text-success' : 'text-text-muted'}`} />
            <span className="flex-1 text-left truncate">
              {controlOn ? t('monitor.control_gate_on') : t('monitor.control_gate')}
            </span>
            <span className={`text-[10px] px-1.5 py-0.5 rounded shrink-0 ${
              controlOn ? 'bg-success-soft-mid text-success' : 'bg-bg-hover text-text-muted'
            }`}>
              {controlOn ? t('monitor.gate_open') : t('monitor.gate_closed')}
            </span>
          </button>
        </Tooltip>
      </div>

      <p className="text-[10px] text-text-muted leading-relaxed whitespace-pre-line">
        {t('gates.hint')}
      </p>
    </RailCard>
  )
}
