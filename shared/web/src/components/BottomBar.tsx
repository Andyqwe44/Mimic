// ═══ BottomBar — responsive status strip ═══
import { Monitor, Camera, Play, ArrowUp } from 'lucide-react'
import { METHOD_SHORT } from '../lib/constants'
import { useTranslation } from 'react-i18next'
import { Tooltip } from './Toolkit'
import { TEXT } from '../lib/design'

export function BottomBar({
  selWin,
  snapMethod,
  streamMethod,
  previewing,
  fps,
  targetDims,
  appVersion,
  agentConnected,
  hasUpdate,
  onCheckUpdate,
}: {
  selWin: string
  snapMethod: string
  streamMethod: string
  previewing: boolean
  fps: number
  targetDims: { w: number; h: number } | null
  appVersion: string
  agentConnected: boolean
  hasUpdate?: boolean
  onCheckUpdate?: () => void
}) {
  const { t } = useTranslation()
  return (
    <div
      className={`flex items-center flex-wrap gap-x-3 gap-y-1 min-h-9 bg-bg-secondary border-t border-border
        px-3 max-[359px]:px-2 shrink-0 ${TEXT.xs} text-text-secondary`}
    >
      <span className="inline-flex items-center gap-1.5 max-w-[40%] min-[600px]:max-w-[200px] truncate">
        <Monitor className="w-3 h-3 text-text-tertiary shrink-0" />
        <span className="truncate">{selWin}</span>
      </span>
      <span className="text-border select-none hidden min-[360px]:inline">│</span>
      <span className="inline-flex items-center gap-1">
        <Camera className="w-3 h-3 text-text-tertiary" />
        <span className="font-medium text-text-primary">
          {METHOD_SHORT[snapMethod] || snapMethod}
        </span>
      </span>
      <span className="inline-flex items-center gap-1">
        <Play className="w-3 h-3 text-text-tertiary" />
        <span className="font-medium text-text-primary">
          {METHOD_SHORT[streamMethod] || streamMethod}
        </span>
        {previewing && (
          <>
            <span className="text-text-muted tabular-nums">{fps}fps</span>
            <span className="w-1.5 h-1.5 rounded-full bg-success animate-pulse" />
          </>
        )}
      </span>
      {targetDims && targetDims.w > 0 && (
        <span className="text-text-muted tabular-nums hidden min-[480px]:inline">
          {targetDims.w}×{targetDims.h}
        </span>
      )}
      <span className="inline-flex items-center gap-1.5 min-w-0">
        <span className="text-text-muted hidden min-[600px]:inline">{t('bottombar.tcp')}</span>
        <span
          className={`w-1.5 h-1.5 rounded-full shrink-0 ${agentConnected ? 'bg-success' : 'bg-text-muted'}`}
        />
        <span className="text-text-muted truncate">
          {agentConnected ? t('bottombar.agent_online') : t('bottombar.waiting_connection')}
        </span>
      </span>
      <span className="flex-1 min-w-2" />
      {hasUpdate && (
        <Tooltip text={t('bottombar.update_title')}>
          <button
            type="button"
            onClick={onCheckUpdate}
            className="inline-flex items-center gap-1 px-1.5 py-0.5 rounded text-[10px] font-medium bg-accent-soft-mid text-accent hover:bg-accent-soft-strong transition-colors cursor-pointer"
          >
            <ArrowUp className="w-3 h-3" />
            {t('bottombar.update')}
          </button>
        </Tooltip>
      )}
      <span className="text-text-muted font-mono text-[11px] shrink-0">{appVersion}</span>
    </div>
  )
}
