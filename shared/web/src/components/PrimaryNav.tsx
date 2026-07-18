// PrimaryNav — side rail (desktop/tablet) or bottom bar (phone). Same IA.
import type { ReactNode } from 'react'
import { Monitor, SlidersHorizontal, FileText, Settings } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Tooltip } from './Toolkit'
import { H, NAV, RADIUS, SHELL_PAD, TEXT } from '../lib/design'
import { PRIMARY_PAGES, type AppPage } from '../lib/pages'
import type { ShellMode } from '../hooks/useViewport'
import { addLog } from '../lib/bridge'

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
  appVersion,
}: {
  page: AppPage
  setPage: (p: AppPage) => void
  mode: ShellMode
  appVersion?: string
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

  if (mode === 'bottom') {
    return (
      <nav
        aria-label={t('nav.aria')}
        className={`shrink-0 grid grid-cols-4 gap-1 px-1.5 pt-1 border-t border-border bg-bg-secondary
          ${NAV.bottomH} ${SHELL_PAD.safeBottom}
          pl-[max(0.375rem,env(safe-area-inset-left,0px))]
          pr-[max(0.375rem,env(safe-area-inset-right,0px))]`}
      >
        {items.map((it) => {
          const active = isActive(it.id)
          return (
            <Tooltip key={it.id} text={it.tip} className="w-full min-w-0">
              <button
                type="button"
                aria-current={active ? 'page' : undefined}
                onClick={() => select(it.id)}
                className={`w-full ${NAV.touchMin} flex flex-col items-center justify-center gap-0.5 px-1
                  ${RADIUS.lg} transition-colors
                  ${active
                    ? 'bg-accent/15 text-accent ring-1 ring-inset ring-accent/30'
                    : 'text-text-secondary active:bg-bg-hover'}`}
              >
                {it.icon}
                <span className={`${TEXT.tiny} font-medium truncate max-w-full`}>{it.label}</span>
              </button>
            </Tooltip>
          )
        })}
      </nav>
    )
  }

  const wide = mode === 'side'
  return (
    <nav
      aria-label={t('nav.aria')}
      className={`shrink-0 flex flex-col border-r border-border bg-bg-secondary
        ${wide ? NAV.sideWide : NAV.sideCompact} ${SHELL_PAD.safeTop}`}
    >
      <div
        className={`px-2 py-3 ${TEXT.xs} font-bold tracking-wide truncate
          ${wide ? 'text-text-primary' : 'text-center text-accent'}`}
      >
        {wide ? 'MIMIC' : 'M'}
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
