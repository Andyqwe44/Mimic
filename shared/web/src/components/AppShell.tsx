// AppShell — unified responsive chrome: side or bottom PrimaryNav + page column.
import type { ReactNode } from 'react'
import { PrimaryNav } from './PrimaryNav'
import { PageHeader } from './PageHeader'
import type { AppPage } from '../lib/pages'
import type { ShellMode } from '../hooks/useViewport'
import { usePageSwipe } from '../hooks/usePageSwipe'

export function AppShell({
  page,
  setPage,
  shellMode,
  pageTitle,
  device,
  connected,
  streaming,
  controlling,
  sessionError,
  compactCapsule,
  short,
  onCapsule,
  appVersion,
  headerTrailing,
  statusBar,
  children,
}: {
  page: AppPage
  setPage: (p: AppPage) => void
  shellMode: ShellMode
  pageTitle: string
  device: string
  connected: boolean
  streaming: boolean
  controlling: boolean
  sessionError?: boolean
  compactCapsule?: boolean
  short?: boolean
  onCapsule: () => void
  appVersion?: string
  headerTrailing?: ReactNode
  statusBar?: ReactNode
  children: ReactNode
}) {
  const bottom = shellMode === 'bottom'
  const pageSwipeRef = usePageSwipe(bottom, page, setPage)

  return (
    <div
      className={`h-full flex bg-bg-primary ${bottom ? 'flex-col' : 'flex-row'}
        ${bottom
          ? 'pl-[env(safe-area-inset-left,0px)] pr-[env(safe-area-inset-right,0px)]'
          : ''}`}
    >
      {!bottom && (
        <PrimaryNav page={page} setPage={setPage} mode={shellMode} appVersion={appVersion} />
      )}
      <div ref={pageSwipeRef} className="flex-1 flex flex-col min-w-0 min-h-0">
        <PageHeader
          page={page}
          title={pageTitle}
          device={device}
          connected={connected}
          streaming={streaming}
          controlling={controlling}
          error={sessionError}
          compact={compactCapsule}
          short={short}
          onCapsule={onCapsule}
          trailing={headerTrailing}
        />
        <main className="flex-1 flex flex-col min-h-0 overflow-hidden">{children}</main>
        {statusBar}
        {bottom && (
          <PrimaryNav page={page} setPage={setPage} mode="bottom" appVersion={appVersion} />
        )}
      </div>
    </div>
  )
}
