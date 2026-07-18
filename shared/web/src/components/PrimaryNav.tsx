// PrimaryNav — side rail (desktop) or bottom bar (phone). Same IA.
import { useLayoutEffect, type ReactNode, type RefObject } from 'react'
import { Monitor, SlidersHorizontal, FileText, Settings } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Tooltip } from './Toolkit'
import { H, NAV, RADIUS, SHELL_PAD, TEXT } from '../lib/design'
import { PRIMARY_PAGES, pageIndex, type AppPage } from '../lib/pages'
import type { ShellMode } from '../hooks/useViewport'
import { addLog } from '../lib/bridge'
import { writeNavProgress } from './PagePager'

const ICONS: Record<string, ReactNode> = {
  Monitor: <Monitor className={H.icon} />,
  Control: <SlidersHorizontal className={H.icon} />,
  Log: <FileText className={H.icon} />,
  Settings: <Settings className={H.icon} />,
}

export function PrimaryNav({
  page,
  setPage,
  mode,
  expanded = true,
  onToggleExpand,
  appVersion,
  progressHostRef,
}: {
  page: AppPage
  setPage: (p: AppPage) => void
  mode: ShellMode
  /** Side rail: show labels when true */
  expanded?: boolean
  onToggleExpand?: () => void
  appVersion?: string
  /** Shared CSS-var host for sliding pill (bottom mode). */
  progressHostRef?: RefObject<HTMLElement | null>
}) {
  const { t } = useTranslation()
  const items = PRIMARY_PAGES.map((id) => ({
    id,
    icon: ICONS[id],
    label: t(`nav.${id.toLowerCase()}`),
    tip: t(`nav.${id.toLowerCase()}_tip`),
  }))

  const select = (id: AppPage) => {
    setPage(id)
    addLog(`[Nav] ${id}`)
  }

  const isActive = (id: AppPage) =>
    page === id || (page === 'DevTools' && id === 'Settings')

  // When pager is not mounted (DevTools overlay / tab click before pager paints),
  // keep pill aligned to the discrete page index.
  useLayoutEffect(() => {
    if (mode !== 'bottom' || !progressHostRef?.current) return
    const host = progressHostRef.current
    if (host.classList.contains('nav-dragging')) return
    writeNavProgress(host, pageIndex(page), false)
  }, [mode, page, progressHostRef])

  if (mode === 'bottom') {
    const gapRem = NAV.bottomGapRem
    const n = PRIMARY_PAGES.length
    return (
      <nav
        aria-label={t('nav.aria')}
        className={`shrink-0 border-t border-border bg-bg-secondary/95 backdrop-blur-sm
          ${SHELL_PAD.safeBottom}
          pl-[max(0.375rem,env(safe-area-inset-left,0px))]
          pr-[max(0.375rem,env(safe-area-inset-right,0px))]`}
      >
        {/* Content row: fixed height; safe-area padding is outside so pill isn't pushed down */}
        <div
          className={`relative grid grid-cols-4 ${NAV.bottomGap} px-1.5 ${NAV.bottomH}`}
        >
          <div
            aria-hidden
            className={`nav-pill pointer-events-none absolute ${NAV.pillTop} ${NAV.pillH}
              ${RADIUS.lg} ${NAV.pillBg} ${NAV.pillRing} will-change-transform`}
            style={{
              width: `calc((100% - ${(n - 1) * gapRem}rem) / ${n})`,
              // translateX % is relative to pill width → slot pitch = width + gap
              transform: `translateX(calc(var(--nav-fraction, ${pageIndex(page)}) * (100% + ${gapRem}rem)))`,
            }}
          />
          {items.map((it) => {
            const active = isActive(it.id)
            return (
              <Tooltip key={it.id} text={it.tip} className="w-full min-w-0 relative z-[1]">
                <button
                  type="button"
                  aria-current={active ? 'page' : undefined}
                  onClick={() => select(it.id)}
                  className={`w-full ${NAV.touchMin} flex flex-col items-center justify-center gap-0.5 px-1
                    ${RADIUS.lg} transition-colors
                    ${active
                      ? 'text-accent'
                      : 'text-text-secondary active:bg-bg-hover/60'}`}
                >
                  {it.icon}
                  <span className={`${TEXT.tiny} font-medium truncate max-w-full`}>{it.label}</span>
                </button>
              </Tooltip>
            )
          })}
        </div>
      </nav>
    )
  }

  const wide = expanded
  return (
    <nav
      aria-label={t('nav.aria')}
      className={`shrink-0 flex flex-col border-r border-border bg-bg-secondary transition-[width] duration-200
        ${wide ? NAV.sideWide : NAV.sideCompact} ${SHELL_PAD.safeTop}`}
    >
      {/* Brand = rail collapse control; same horizontal inset as nav items below */}
      <div className="px-1.5 pt-2 pb-1">
        <Tooltip text={wide ? t('nav.collapse') : t('nav.expand')} className="w-full">
          <button
            type="button"
            aria-expanded={wide}
            aria-label={wide ? t('nav.collapse') : t('nav.expand')}
            onClick={() => {
              onToggleExpand?.()
              addLog(`[Nav] rail ${wide ? 'collapse' : 'expand'}`)
            }}
            className={`w-full ${NAV.touchMin} flex items-center gap-2 ${RADIUS.lg} px-2 py-2
              text-text-primary hover:bg-bg-hover transition-colors
              ${wide ? '' : 'justify-center'}`}
          >
            {/* Mark slot aligns with page icons; later: wide logo vs square icon */}
            <span
              className={`${H.icon} flex items-center justify-center shrink-0
                ${TEXT.xs} font-bold tracking-tight text-accent`}
              aria-hidden
            >
              M
            </span>
            {wide && (
              <span className={`${TEXT.xs} font-bold tracking-wide truncate`}>MIMIC</span>
            )}
          </button>
        </Tooltip>
      </div>

      <div className="flex-1 flex flex-col gap-1 px-1.5">
        {items.map((it) => {
          const active = isActive(it.id)
          const btn = (
            <button
              type="button"
              aria-current={active ? 'page' : undefined}
              onClick={() => select(it.id)}
              className={`relative w-full ${NAV.touchMin} flex items-center gap-2 ${RADIUS.lg} px-2 py-2
                transition-colors
                ${active
                  ? 'bg-accent/15 text-accent'
                  : 'text-text-secondary hover:bg-bg-hover hover:text-text-primary'}
                ${wide ? '' : 'justify-center'}`}
            >
              {active && (
                <span className="absolute left-0 top-2 bottom-2 w-0.5 rounded-r bg-accent" />
              )}
              {it.icon}
              {wide && <span className={`${TEXT.xs} font-medium`}>{it.label}</span>}
            </button>
          )
          return (
            <Tooltip key={it.id} text={it.tip} className="w-full">
              {btn}
            </Tooltip>
          )
        })}
      </div>
      {appVersion && (
        <div className={`px-2 py-2 ${TEXT.tiny} text-text-muted font-mono truncate text-center`}>
          {wide ? appVersion : appVersion.replace(/^v/, '')}
        </div>
      )}
    </nav>
  )
}
