// Shared right-rail card chrome: title + badges + collapse + pin (Connection style).
import type { ReactNode } from 'react'
import { ChevronDown, Pin } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Tooltip } from './Toolkit'
import { CollapsibleBody } from './CollapsibleBody'
import { COLLAPSIBLE_HEADER } from '../lib/constants'

export type RailBadgeTone = 'accent' | 'success' | 'warn' | 'error' | 'muted'

/** Soft fills use CSS vars (rgba) — avoid Tailwind opacity color-mix on Android WebView. */
const BADGE_TONE: Record<RailBadgeTone, string> = {
  accent: 'text-accent bg-[var(--color-accent-soft)]',
  success: 'text-emerald-500 bg-[var(--color-success-soft)]',
  warn: 'text-amber-500 bg-[var(--color-warn-soft)]',
  error: 'text-error bg-[var(--color-error-soft)]',
  muted: 'text-text-muted bg-bg-tertiary',
}

export function RailBadge({
  text,
  tone = 'accent',
}: {
  text: string
  tone?: RailBadgeTone
}) {
  if (!text) return null
  return (
    <span className={`text-[11px] font-medium px-1.5 py-0.5 rounded shrink-0 max-w-[9rem] truncate ${BADGE_TONE[tone]}`}>
      {text}
    </span>
  )
}

export function RailCard({
  icon,
  title,
  badges,
  expanded,
  onToggle,
  pinned,
  onTogglePin,
  children,
  maxBodyClass = 'max-h-[360px]',
}: {
  icon: ReactNode
  title: string
  badges?: Array<{ text: string; tone?: RailBadgeTone }>
  expanded: boolean
  onToggle: () => void
  pinned?: boolean
  onTogglePin?: () => void
  children: ReactNode
  maxBodyClass?: string
}) {
  const { t } = useTranslation()
  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden min-w-0">
      <div
        role="button"
        tabIndex={0}
        onClick={onToggle}
        onKeyDown={(e) => {
          if (e.key === 'Enter' || e.key === ' ') (e.currentTarget as HTMLElement).click()
        }}
        className={COLLAPSIBLE_HEADER}
      >
        <div className="flex items-center gap-2 min-w-0 flex-1">
          <span className="shrink-0">{icon}</span>
          <span className="text-sm font-medium text-text-primary shrink-0">{title}</span>
          <div className="flex items-center gap-1 min-w-0 flex-1 flex-wrap">
            {(badges || []).filter((b) => b.text).map((b, i) => (
              <RailBadge key={`${b.text}-${i}`} text={b.text} tone={b.tone} />
            ))}
          </div>
        </div>
        <div className="flex items-center gap-1.5 ml-2 shrink-0">
          {onTogglePin && (
            <Tooltip text={pinned ? t('connection.unpin_tip') : t('connection.pin_tip')}>
              <button
                type="button"
                onClick={(e) => {
                  e.stopPropagation()
                  onTogglePin()
                }}
                className={`p-1 rounded-md transition-colors ${
                  pinned ? 'text-accent hover:bg-bg-tertiary' : 'text-text-secondary hover:text-text-primary hover:bg-bg-tertiary'
                }`}
              >
                <Pin className={`w-3.5 h-3.5 ${pinned ? 'fill-current' : ''}`} />
              </button>
            </Tooltip>
          )}
          <ChevronDown
            className={`w-4 h-4 text-text-muted transition-transform duration-150 ${expanded ? 'rotate-180' : ''}`}
          />
        </div>
      </div>
      <CollapsibleBody expanded={expanded} maxPx={480}>
        <div data-layout-measure="">
          <div className="border-t border-border" />
          <div className={`${maxBodyClass} overflow-y-auto overflow-x-hidden p-3 space-y-2 min-w-0`}>
            {children}
          </div>
        </div>
      </CollapsibleBody>
    </div>
  )
}
