// PageHeader — title (+ optional subtitle) + trailing + SessionCapsule.
import type { ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { TEXT } from '../lib/design'
import { SessionCapsule } from './SessionCapsule'
import type { AppPage } from '../lib/pages'

export function PageHeader({
  page,
  title,
  device,
  connected,
  streaming,
  controlling,
  error,
  compact,
  short,
  onCapsule,
  trailing,
}: {
  page: AppPage
  title: string
  device: string
  connected: boolean
  streaming: boolean
  controlling: boolean
  error?: boolean
  compact?: boolean
  /** Short viewport — compress height / hide subtitle */
  short?: boolean
  onCapsule: () => void
  trailing?: ReactNode
}) {
  const { t } = useTranslation()

  let statusHint = t('capsule.disconnected')
  if (error) statusHint = t('capsule.error')
  else if (controlling) statusHint = t('capsule.control_on')
  else if (streaming) statusHint = t('capsule.stream_on')
  else if (connected) statusHint = t('capsule.connected')

  const showSub = !short && !compact

  return (
    <header
      className={`shrink-0 flex items-center gap-2 px-3 max-[359px]:px-2
        border-b border-border bg-bg-secondary pt-[env(safe-area-inset-top,0px)]
        ${short ? 'h-10 min-h-10' : showSub ? 'h-12 min-h-12' : 'h-11 min-h-11'}`}
    >
      <div className="min-w-0 flex flex-col justify-center">
        <h1 className={`${TEXT.sm} font-semibold text-text-primary truncate leading-tight`}>
          {title}
          {page === 'DevTools' && (
            <span className={`${TEXT.tiny} text-text-muted font-normal ml-1.5`}>Dev</span>
          )}
        </h1>
        {showSub && (
          <p className={`${TEXT.tiny} text-text-tertiary truncate leading-tight`}>
            <span className="text-text-secondary">{device}</span>
            <span className="mx-1 text-border">·</span>
            {statusHint}
          </p>
        )}
      </div>
      <div className="flex-1" />
      {trailing}
      <SessionCapsule
        device={device}
        connected={connected}
        streaming={streaming}
        controlling={controlling}
        error={error}
        compact={compact || short}
        onClick={onCapsule}
      />
    </header>
  )
}
