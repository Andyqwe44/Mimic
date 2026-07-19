// HeaderActions — compact lang / permission / theme / update strip (from former TopBar).
import { useEffect, useRef, useState } from 'react'
import { User, Shield, RefreshCw } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { ThemeBtn, Tooltip } from './Toolkit'
import { addLog } from '../lib/bridge'
import { H, PAD, RADIUS, TEXT } from '../lib/design'

const LOCALES: Array<{ code: string; abbr: string; tipKey: string }> = [
  { code: 'en', abbr: 'En', tipKey: 'settings.language_en' },
  { code: 'zh-CN', abbr: '简', tipKey: 'settings.language_zh_cn' },
  { code: 'zh-TW', abbr: '繁', tipKey: 'settings.language_zh_tw' },
]

function localeAbbr(code: string): string {
  return LOCALES.find((l) => l.code === code)?.abbr ?? 'En'
}

const ICON_CELL = `p-2 ${RADIUS.md} hover:bg-bg-hover transition-colors text-text-secondary`

export function HeaderActions({
  dark,
  onToggleTheme,
  locale,
  setLocale,
  isAdmin,
  onSwitchPermission,
  hidePermission,
  compact,
  onCheckUpdate,
  hasUpdate,
  updateLatest,
}: {
  dark: boolean
  onToggleTheme: () => void
  locale: string
  setLocale: (l: string) => void
  isAdmin: boolean
  onSwitchPermission: (toAdmin: boolean) => void
  /** Android: no UAC elevation — hide Windows admin toggle */
  hidePermission?: boolean
  compact?: boolean
  /** Always-reachable update entry (escape hatch if Settings scroll/layout fails). */
  onCheckUpdate?: () => void
  hasUpdate?: boolean
  /** Remote version string when hasUpdate (no leading v). */
  updateLatest?: string
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

  return (
    <div className={`flex items-center ${compact ? 'gap-0' : 'gap-0.5'} shrink-0`}>
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
                  ${locale === code ? 'bg-accent-soft-mid text-accent' : 'text-text-secondary hover:bg-bg-hover'}`}
              >
                <span className="inline-block w-6">{abbr}</span>
                <span className={`${TEXT.tiny} text-text-muted font-normal`}>{t(tipKey)}</span>
              </button>
            ))}
          </div>
        )}
      </div>

      {!hidePermission && (
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
      )}

      <ThemeBtn dark={dark} onToggle={onToggleTheme} />

      {onCheckUpdate && (
        <div className="relative flex flex-col items-center">
          <Tooltip text={hasUpdate ? t('settings.check_update_latest_tip') : t('settings.check_update_tip')}>
            <button
              type="button"
              onClick={() => {
                onCheckUpdate()
                addLog('[Update] header check')
              }}
              className={`${ICON_CELL} relative`}
              aria-label={hasUpdate ? t('settings.check_update_latest') : t('settings.check_update')}
            >
              <RefreshCw className={`${H.icon} ${hasUpdate ? 'text-accent' : ''}`} />
              {hasUpdate && (
                <span
                  aria-hidden
                  className="absolute top-1.5 right-1.5 w-1.5 h-1.5 rounded-full bg-accent"
                />
              )}
            </button>
          </Tooltip>
          {hasUpdate && (
            <button
              type="button"
              onClick={() => {
                onCheckUpdate()
                addLog('[Update] header bubble')
              }}
              className={`absolute top-full mt-0.5 z-40 max-w-[11rem] px-1.5 py-0.5 ${RADIUS.md}
                bg-accent text-white ${TEXT.tiny} font-medium leading-snug text-center
                shadow-md whitespace-nowrap truncate cursor-pointer`}
            >
              {t('settings.update_bubble', { version: updateLatest || '?' })}
            </button>
          )}
        </div>
      )}
    </div>
  )
}
