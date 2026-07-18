// Shared expand/collapse body — max-height fallback so Android WebView still animates
// when grid-template-rows 0fr/1fr transitions are not interpolated.
import type { ReactNode } from 'react'

export function CollapsibleBody({
  expanded,
  children,
  /** Upper bound for the max-height fallback path (px). */
  maxPx = 720,
}: {
  expanded: boolean
  children: ReactNode
  maxPx?: number
}) {
  return (
    <div
      className="collapsible-body"
      data-open={expanded ? 'true' : 'false'}
      style={{ ['--collapsible-max' as string]: `${maxPx}px` }}
    >
      <div className="collapsible-body-inner min-h-0 overflow-hidden">
        {children}
      </div>
    </div>
  )
}
