// ═══ TopBar — MXU-style tab bar with Start/Stop + locale/perm/theme shortcuts ═══
import { useEffect, useRef, useState } from 'react'
import { FileText, Monitor, Settings, Cpu, Play, Square, User, Shield } from 'lucide-react'
import { ActionBtn, ThemeBtn, Tooltip } from './Toolkit'
import { addLog } from '../lib/bridge'
import { useTranslation } from 'react-i18next'
import { GAP, H, PAD, RADIUS, TEXT } from '../lib/design'

const LOCALES: Array<{ code: string; abbr: string; tipKey: string }> = [
  { code: 'en', abbr: 'En', tipKey: 'settings.language_en' },
  { code: 'zh-CN', abbr: '简', tipKey: 'settings.language_zh_cn' },
  { code: 'zh-TW', abbr: '繁', tipKey: 'settings.language_zh_tw' },
]

function localeAbbr(code: string): string {
  return LOCALES.find((l) => l.code === code)?.abbr ?? 'En'
}

/** Same hit-box as ThemeBtn — one cell in the top-right strip. */
const ICON_CELL = `p-2 ${RADIUS.md} hover:bg-bg-hover transition-colors text-text-secondary`

export function TopBar({
  tab,
  setTab,
  running,
  onStart,
  onStop,
  dark,
  onToggleTheme,
  devMode,
  locale,
  setLocale,
  isAdmin,
  onSwitchPermission,
}: {
  tab: string
  setTab: (t: 'Monitor' | 'Log' | 'Settings' | 'DevTools') => void
  running: boolean
  onStart: () => void
  onStop: () => void
  dark: boolean
  onToggleTheme: () => void
  devMode: boolean
  locale: string
  setLocale: (l: string) => void
  isAdmin: boolean
  onSwitchPermission: (toAdmin: boolean) => void
}) {
  const { t } = useTranslation()
  const [langOpen, setLangOpen] = useState(false)
  const langRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!langOpen) return
    const onDoc = (e: MouseEvent) => {
      if (!langRef.current?.contains(e.target as Node)) setLangOpen(false)
    }
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setLangOpen(false)
    }
    document.addEventListener('mousedown', onDoc)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('mousedown', onDoc)
      document.removeEventListener('keydown', onKey)
    }
  }, [langOpen])

  const tabs: Array<{ id: 'Monitor' | 'Log' | 'Settings' | 'DevTools'; icon: React.ReactNode; label: string; tip: string }> = [
    { id: 'Monitor' as const, icon: <Monitor className={H.iconSm} />, label: t('topbar.monitor'), tip: t('topbar.monitor_tip') },
    { id: 'Log' as const, icon: <FileText className={H.iconSm} />, label: t('topbar.log'), tip: t('topbar.log_tip') },
    {
      id: 'Settings' as const,
      icon: <Settings className={H.iconSm} />,
      label: t('topbar.settings'),
      tip: t('topbar.settings_tip'),
    },
  ]
  if (devMode) {
    tabs.push({
      id: 'DevTools' as const,
      icon: <Cpu className={H.iconSm} />,
      label: t('topbar.devtools'),
      tip: t('topbar.devtools_tip'),
    })
  }

  return (
    <div className="flex items-center h-10 bg-bg-secondary border-b border-border select-none shrink-0">
      <div className="flex-1 flex items-center h-full overflow-x-auto">
        {tabs.map((tb) => (
          <Tooltip key={tb.id} text={tb.tip}>
            <button
              onClick={() => {
                setTab(tb.id)
                addLog(`[Tab] ${tb.label}`)
              }}
              className={`group flex items-center gap-1.5 h-full px-3 cursor-pointer border-r border-border border-b-[3px] min-w-[100px] transition-colors
                ${tb.id === tab ? 'bg-bg-primary text-accent border-b-accent' : 'bg-bg-tertiary text-text-secondary hover:bg-bg-hover border-b-transparent'}`}
            >
              {tb.icon}
              <span className={`${TEXT.sm} font-medium`}>{tb.label}</span>
            </button>
          </Tooltip>
        ))}
      </div>

      {/* Right: Start/Stop | Lang · Perm · Theme (one divider only) */}
      <div className={`flex items-center ${GAP.xs} px-2`}>
        {running ? (
          <ActionBtn
            icon={<Square className={H.iconSm} />}
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
            icon={<Play className={H.iconSm} />}
            label={t('topbar.start')}
            title={t('topbar.start_tip')}
            variant="primary"
            onClick={() => {
              onStart()
              addLog('[Action] Start')
            }}
          />
        )}

        <div className="mx-1 h-4 w-px bg-border shrink-0" />

        {/* Language — one cell + dropdown */}
        <div className="relative" ref={langRef}>
          <Tooltip text={t('settings.language')}>
            <button
              type="button"
              onClick={() => setLangOpen((v) => !v)}
              className={`${ICON_CELL} min-w-8 ${TEXT.xs} font-semibold`}
              aria-expanded={langOpen}
              aria-haspopup="listbox"
            >
              {localeAbbr(locale)}
            </button>
          </Tooltip>
          {langOpen && (
            <div
              role="listbox"
              className={`absolute right-0 top-full mt-1 z-50 min-w-16 bg-bg-secondary border border-border ${RADIUS.lg} shadow-lg overflow-hidden`}
            >
              {LOCALES.map(({ code, abbr, tipKey }) => (
                <button
                  key={code}
                  type="button"
                  role="option"
                  aria-selected={locale === code}
                  onClick={() => {
                    setLangOpen(false)
                    if (locale === code) return
                    setLocale(code)
                    addLog(`[Lang] ${code}`)
                  }}
                  className={`w-full ${PAD.sm} ${TEXT.xs} font-semibold text-left transition-colors
                    ${locale === code ? 'bg-accent/15 text-accent' : 'text-text-secondary hover:bg-bg-hover'}`}
                >
                  <span className="inline-block w-6">{abbr}</span>
                  <span className={`${TEXT.tiny} text-text-muted font-normal`}>{t(tipKey)}</span>
                </button>
              ))}
            </div>
          )}
        </div>

        {/* Permission — one icon cell, click toggles 普 ↔ 管 */}
        <Tooltip text={isAdmin ? t('settings.permission_admin_tip') : t('settings.permission_normal_tip')}>
          <button
            type="button"
            onClick={() => {
              const next = !isAdmin
              onSwitchPermission(next)
              addLog(`[Perm] → ${next ? 'admin' : 'normal'}`)
            }}
            className={ICON_CELL}
          >
            {isAdmin ? (
              <Shield className={`${H.icon} text-accent`} />
            ) : (
              <User className={H.icon} />
            )}
          </button>
        </Tooltip>

        <ThemeBtn dark={dark} onToggle={onToggleTheme} />
      </div>
    </div>
  )
}
