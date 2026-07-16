// ═══ Stream / Control gates — thin-client right-rail card ═══
import { Play, Square, Power } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Tooltip } from './Toolkit'
import { COLLAPSIBLE_HEADER } from '../lib/constants'

export function StreamGatesPanel({
  streamOn,
  controlOn,
  onToggleStream,
  onToggleControl,
  targetTitle,
}: {
  streamOn: boolean
  controlOn: boolean
  onToggleStream: () => void
  onToggleControl: () => void
  targetTitle: string
}) {
  const { t } = useTranslation()

  return (
    <div className="rounded-xl bg-bg-secondary ring-1 ring-inset ring-border overflow-hidden">
      <div className={COLLAPSIBLE_HEADER}>
        <span className="text-xs font-semibold text-text-secondary tracking-wide">
          {t('gates.title')}
        </span>
      </div>
      <div className="px-3 pb-3 space-y-3">
        <div className="text-[11px] text-text-tertiary truncate">
          {t('gates.target')}: {targetTitle}
        </div>

        <div className="flex flex-col gap-2">
          <Tooltip text={t('monitor.stream_gate_tip')}>
            <button
              type="button"
              onClick={onToggleStream}
              className={`w-full flex items-center gap-2 px-3 py-2.5 rounded-lg text-sm font-medium transition-colors ${
                streamOn
                  ? 'bg-accent/15 text-accent ring-1 ring-accent/40'
                  : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'
              }`}
            >
              {streamOn ? <Square className="w-4 h-4 shrink-0" /> : <Play className="w-4 h-4 shrink-0" />}
              <span className="flex-1 text-left">
                {streamOn ? t('monitor.stream_gate_on') : t('monitor.stream_gate')}
              </span>
              <span className={`text-[10px] px-1.5 py-0.5 rounded ${
                streamOn ? 'bg-accent/20 text-accent' : 'bg-bg-hover text-text-muted'
              }`}>
                {streamOn ? t('monitor.gate_open') : t('monitor.gate_closed')}
              </span>
            </button>
          </Tooltip>

          <Tooltip text={t('monitor.control_gate_tip')}>
            <button
              type="button"
              onClick={onToggleControl}
              className={`w-full flex items-center gap-2 px-3 py-2.5 rounded-lg text-sm font-medium transition-colors ${
                controlOn
                  ? 'bg-success/15 text-success ring-1 ring-success/40'
                  : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover'
              }`}
            >
              <Power className={`w-4 h-4 shrink-0 ${controlOn ? 'text-success' : 'text-text-muted'}`} />
              <span className="flex-1 text-left">
                {controlOn ? t('monitor.control_gate_on') : t('monitor.control_gate')}
              </span>
              <span className={`text-[10px] px-1.5 py-0.5 rounded ${
                controlOn ? 'bg-success/20 text-success' : 'bg-bg-hover text-text-muted'
              }`}>
                {controlOn ? t('monitor.gate_open') : t('monitor.gate_closed')}
              </span>
            </button>
          </Tooltip>
        </div>

        <p className="text-[10px] text-text-muted leading-relaxed whitespace-pre-line">
          {t('gates.hint')}
        </p>
      </div>
    </div>
  )
}
