// Shizuku setup guide — secondary modal (MAA-Meow-style steps).
// Must portal to body: PagePager uses transform, which breaks position:fixed.
import type { ReactNode } from 'react'
import { createPortal } from 'react-dom'
import { useTranslation } from 'react-i18next'
import { X, Shield, Download, Smartphone, KeyRound, Link2, ExternalLink, BookOpen } from 'lucide-react'
import { hostCall, addLog } from '../lib/bridge'
import { ActionBtn } from './Toolkit'
import { useScrollLock } from '../lib/useScrollLock'
import { MODAL_W, GAP, PAD, TEXT, RADIUS, RING } from '../lib/design'

type Step = {
  icon: ReactNode
  titleKey: string
  bodyKey: string
}

export function ShizukuGuideModal({
  open,
  onClose,
  onOpenApp,
}: {
  open: boolean
  onClose: () => void
  onOpenApp: () => void
}) {
  useScrollLock(open)
  const { t } = useTranslation()
  if (!open) return null

  const steps: Step[] = [
    {
      icon: <Download className="w-4 h-4" />,
      titleKey: 'peer.shizuku_guide_s1_title',
      bodyKey: 'peer.shizuku_guide_s1_body',
    },
    {
      icon: <Smartphone className="w-4 h-4" />,
      titleKey: 'peer.shizuku_guide_s2_title',
      bodyKey: 'peer.shizuku_guide_s2_body',
    },
    {
      icon: <KeyRound className="w-4 h-4" />,
      titleKey: 'peer.shizuku_guide_s3_title',
      bodyKey: 'peer.shizuku_guide_s3_body',
    },
    {
      icon: <Link2 className="w-4 h-4" />,
      titleKey: 'peer.shizuku_guide_s4_title',
      bodyKey: 'peer.shizuku_guide_s4_body',
    },
  ]

  const openOfficial = async () => {
    try {
      // Prefer native open (market / installed app); falls back to official site.
      const res = await hostCall('open_shizuku')
      if (res?.ok === false) {
        addLog(`[Shizuku] guide open failed: ${res.error}`)
      }
    } catch (e) {
      addLog(`[Shizuku] guide open failed: ${e}`)
    }
  }

  return createPortal(
    <div
      className="fixed inset-0 z-[70] flex items-center justify-center bg-scrim backdrop-blur-sm p-3"
      onClick={onClose}
      role="presentation"
      data-no-page-swipe
    >
      <div
        className={`${MODAL_W.picker} max-h-[min(560px,88vh)] bg-bg-secondary ${RADIUS.xl} ${RING} shadow-2xl flex flex-col overflow-hidden`}
        onClick={(e) => e.stopPropagation()}
        role="dialog"
        aria-modal="true"
        aria-labelledby="shizuku-guide-title"
      >
        <div className={`flex items-center ${GAP.md} ${PAD.lg} border-b border-border shrink-0`}>
          <span className="w-8 h-8 rounded-lg bg-accent-soft flex items-center justify-center text-accent shrink-0">
            <BookOpen className="w-4 h-4" />
          </span>
          <div className="min-w-0 flex-1">
            <div id="shizuku-guide-title" className={`${TEXT.sm} font-semibold text-text-primary`}>
              {t('peer.shizuku_guide_title')}
            </div>
            <div className={`${TEXT.tiny} text-text-muted truncate`}>
              {t('peer.shizuku_guide_subtitle')}
            </div>
          </div>
          <button
            type="button"
            className="p-1.5 rounded-md text-text-muted hover:text-text-primary hover:bg-bg-hover transition-colors"
            onClick={onClose}
            aria-label={t('peer.shizuku_guide_close')}
          >
            <X className="w-4 h-4" />
          </button>
        </div>

        <div className={`flex-1 overflow-y-auto ${PAD.lg} space-y-3 min-h-0`}>
          <p className={`${TEXT.xs} text-text-secondary leading-relaxed`}>
            {t('peer.shizuku_guide_intro')}
          </p>
          <ol className="space-y-2.5">
            {steps.map((s, i) => (
              <li
                key={s.titleKey}
                className={`flex ${GAP.md} ${PAD.md} ${RADIUS.lg} bg-bg-tertiary ring-1 ring-inset ring-border`}
              >
                <span className="w-8 h-8 rounded-md bg-accent-soft text-accent flex items-center justify-center shrink-0">
                  {s.icon}
                </span>
                <div className="min-w-0 flex-1 space-y-0.5">
                  <div className={`${TEXT.xs} font-medium text-text-primary`}>
                    <span className="text-accent tabular-nums mr-1.5">{i + 1}.</span>
                    {t(s.titleKey)}
                  </div>
                  <div className={`${TEXT.tiny} text-text-muted leading-relaxed`}>
                    {t(s.bodyKey)}
                  </div>
                </div>
              </li>
            ))}
          </ol>
          <p className={`${TEXT.tiny} text-text-tertiary leading-relaxed`}>
            {t('peer.shizuku_guide_note')}
          </p>
        </div>

        <div className={`shrink-0 border-t border-border ${PAD.lg} grid grid-cols-2 ${GAP.md}`}>
          <ActionBtn
            icon={<ExternalLink className="w-3.5 h-3.5" />}
            label={t('peer.shizuku_guide_download')}
            title={t('peer.shizuku_guide_download_tip')}
            variant="outline"
            size="fill"
            onClick={() => { void openOfficial() }}
          />
          <ActionBtn
            icon={<Shield className="w-3.5 h-3.5" />}
            label={t('peer.shizuku_open_app')}
            title={t('peer.shizuku_open_app_tip')}
            variant="primary"
            size="fill"
            onClick={() => {
              onOpenApp()
              onClose()
            }}
          />
        </div>
      </div>
    </div>,
    document.body,
  )
}
