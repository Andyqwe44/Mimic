// ═══ TopBar — MXU-style tab bar with Start/Stop + theme toggle ═══
import { FileText, Monitor, Settings, Cpu } from 'lucide-react'
import { ActionBtn, ThemeBtn, Tooltip } from './Toolkit'
import { addLog } from '../lib/bridge'
import { Play, Square } from 'lucide-react'
import { useTranslation } from 'react-i18next'

export function TopBar({
  tab,
  setTab,
  running,
  onStart,
  onStop,
  dark,
  onToggleTheme,
  devMode,
}: {
  tab: string
  setTab: (t: 'Monitor' | 'Log' | 'Settings' | 'DevTools') => void
  running: boolean
  onStart: () => void
  onStop: () => void
  dark: boolean
  onToggleTheme: () => void
  devMode: boolean
}) {
  const { t } = useTranslation()
  // ── Tab definitions ──
  const tabs: Array<{ id: 'Monitor' | 'Log' | 'Settings' | 'DevTools'; icon: React.ReactNode; label: string; tip: string }> = [
    { id: 'Monitor' as const, icon: <Monitor className="w-3.5 h-3.5" />, label: t('topbar.monitor'), tip: t('topbar.monitor_tip') },
    { id: 'Log' as const, icon: <FileText className="w-3.5 h-3.5" />, label: t('topbar.log'), tip: t('topbar.log_tip') },
    {
      id: 'Settings' as const,
      icon: <Settings className="w-3.5 h-3.5" />,
      label: t('topbar.settings'),
      tip: t('topbar.settings_tip'),
    },
  ]
  if (devMode) {
    tabs.push({
      id: 'DevTools' as const,
      icon: <Cpu className="w-3.5 h-3.5" />,
      label: t('topbar.devtools'),
      tip: t('topbar.devtools_tip'),
    })
  }
  return (
    <div className="flex items-center h-10 bg-bg-secondary border-b border-border select-none shrink-0">
      {/* ── Tab buttons ── */}
      <div className="flex-1 flex items-center h-full overflow-x-auto">
        {tabs.map((t) => (
          <Tooltip key={t.id} text={t.tip}>
            <button
              onClick={() => {
                setTab(t.id)
                addLog(`[Tab] ${t.label}`)
              }}
              className={`group flex items-center gap-1.5 h-full px-3 cursor-pointer border-r border-border border-b-[3px] min-w-[100px] transition-colors
                ${t.id === tab ? 'bg-bg-primary text-accent border-b-accent' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover border-b-transparent'}`}
            >
              {t.icon}
              <span className="text-sm font-medium">{t.label}</span>
            </button>
          </Tooltip>
        ))}
      </div>
      {/* ── Right: Start/Stop + Theme ── */}
      <div className="flex items-center gap-1 px-2">
        {running ? (
          <ActionBtn
            icon={<Square className="w-3.5 h-3.5" />}
            label={t('topbar.stop')}
            title={t('topbar.stop_tip')}
            variant="danger"
            onClick={() => {
              onStop()
              addLog('[Action] Stop')
            }}
          />
        ) : (
          <ActionBtn
            icon={<Play className="w-3.5 h-3.5" />}
            label={t('topbar.start')}
            title={t('topbar.start_tip')}
            variant="primary"
            onClick={() => {
              onStart()
              addLog('[Action] Start')
            }}
          />
        )}
        <div className="mx-1 h-4 w-px bg-border" />
        <ThemeBtn dark={dark} onToggle={onToggleTheme} />
      </div>
    </div>
  )
}
