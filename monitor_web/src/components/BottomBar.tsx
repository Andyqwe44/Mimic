// ═══ BottomBar — status strip: target, methods, FPS, dims, TCP, version ═══
import { Monitor, Camera, Play, ArrowUp } from 'lucide-react'
import { METHOD_SHORT } from '../lib/constants'
import { useTranslation } from 'react-i18next'

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
  targetDims: {w: number; h: number} | null
  appVersion: string
  agentConnected: boolean
  hasUpdate?: boolean
  onCheckUpdate?: () => void
}) {
  const { t } = useTranslation()
  return (
    <div className="flex items-center h-9 bg-bg-secondary border-t border-border px-4 shrink-0 gap-3 text-xs text-text-secondary">
      {/* Target window */}
      <span className="inline-flex items-center gap-1.5 max-w-[200px] truncate">
        <Monitor className="w-3 h-3 text-text-tertiary shrink-0" />
        <span className="truncate">{selWin}</span>
      </span>
      <span className="text-border/40 select-none">│</span>
      {/* Capture methods */}
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
            <span className="text-text-muted">{fps}fps</span>
            <span className="w-1.5 h-1.5 rounded-full bg-success animate-pulse" />
          </>
        )}
      </span>
      {targetDims && targetDims.w > 0 && (
        <>
          <span className="text-border/40 select-none">│</span>
          <span className="text-text-muted">{targetDims.w}×{targetDims.h}</span>
        </>
      )}
      <span className="text-border/40 select-none">│</span>
      {/* Agent TCP connection */}
      <span className="inline-flex items-center gap-1.5">
        <span className="text-text-muted">{t('bottombar.tcp')}</span>
        <span
          className={`w-1.5 h-1.5 rounded-full ${agentConnected ? 'bg-success' : 'bg-text-muted'}`}
        />
        <span className="text-text-muted">
          {agentConnected ? t('bottombar.agent_online') : t('bottombar.waiting_connection')}
        </span>
      </span>
      <span className="flex-1" />
      {/* App version */}
      {hasUpdate && (
        <button
          onClick={onCheckUpdate}
          className="inline-flex items-center gap-1 px-1.5 py-0.5 rounded text-[10px] font-medium bg-accent/15 text-accent hover:bg-accent/25 transition-colors cursor-pointer"
          title={t('bottombar.update_title')}
        >
          <ArrowUp className="w-3 h-3" />
          {t('bottombar.update')}
        </button>
      )}
      <span className="text-text-muted font-mono text-[11px]">{appVersion}</span>
    </div>
  )
}
