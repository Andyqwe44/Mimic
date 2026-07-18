// ═══ Tooltip, ActionBtn, ThemeBtn — reusable UI kit ═══
import { useState, useRef, useLayoutEffect } from 'react'
import { createPortal } from 'react-dom'
import { Moon, Sun } from 'lucide-react'
import type { ReactNode } from 'react'
import { useTranslation } from 'react-i18next'
import { BTN_SIZE_CLASS, btnAutoSize, H, RADIUS, TEXT } from '../lib/design'

const TIP_GAP = 6
const TIP_MARGIN = 4
const PAD_TIP = 'px-2 py-1'

type TipPlacement = 'top' | 'bottom'

/**
 * Tooltip positioning (铁律 4):
 * 1. Prefer above the anchor.
 * 2. If anchor is near the window top (not enough room above) → below.
 * 3. Prefer horizontally centered on the anchor; clamp so the tip stays fully
 *    inside the viewport (right/left edges), without drifting away needlessly.
 * Uses real measured tip size — never estimate from string length (EN tips are
 * longer than ZH and the old estimate pulled them far left).
 */
export function Tooltip({
  text,
  children,
  className,
}: {
  text: string
  children: React.ReactElement
  className?: string
}) {
  const [show, setShow] = useState(false)
  const [pos, setPos] = useState({ left: 0, top: 0, placement: 'top' as TipPlacement })
  const [ready, setReady] = useState(false)
  const timer = useRef<number>(0)
  const anchorRef = useRef<HTMLDivElement>(null)
  const tipRef = useRef<HTMLDivElement>(null)

  const placeTip = () => {
    const anchorEl = anchorRef.current
    const tipEl = tipRef.current
    if (!anchorEl || !tipEl) return

    const a = anchorEl.getBoundingClientRect()
    const tipW = tipEl.offsetWidth
    const tipH = tipEl.offsetHeight
    const vw = window.innerWidth
    const vh = window.innerHeight

    // ── Vertical: prefer above; flip below when top-clamped ──
    let placement: TipPlacement = 'top'
    let top = a.top - TIP_GAP - tipH
    if (a.top < tipH + TIP_GAP + TIP_MARGIN) {
      placement = 'bottom'
      top = a.bottom + TIP_GAP
    }
    // Keep fully inside viewport vertically
    if (top + tipH > vh - TIP_MARGIN) top = Math.max(TIP_MARGIN, vh - tipH - TIP_MARGIN)
    if (top < TIP_MARGIN) {
      if (a.bottom + TIP_GAP + tipH <= vh - TIP_MARGIN) {
        placement = 'bottom'
        top = a.bottom + TIP_GAP
      } else {
        top = TIP_MARGIN
      }
    }

    // ── Horizontal: center on anchor, clamp into viewport ──
    let left = a.left + a.width / 2 - tipW / 2
    if (left + tipW > vw - TIP_MARGIN) left = vw - tipW - TIP_MARGIN
    if (left < TIP_MARGIN) left = TIP_MARGIN

    setPos({ left, top, placement })
    setReady(true)
  }

  useLayoutEffect(() => {
    if (!show) {
      setReady(false)
      return
    }
    placeTip()
    const onResize = () => placeTip()
    window.addEventListener('resize', onResize)
    window.addEventListener('scroll', onResize, true)
    return () => {
      window.removeEventListener('resize', onResize)
      window.removeEventListener('scroll', onResize, true)
    }
  }, [show, text])

  return (
    <div
      ref={anchorRef}
      className={`relative inline-flex ${className || ''}`}
      onMouseEnter={() => {
        timer.current = window.setTimeout(() => setShow(true), 300)
      }}
      onMouseLeave={() => {
        clearTimeout(timer.current)
        setShow(false)
        setReady(false)
      }}
      onMouseMove={() => {
        if (!show) {
          clearTimeout(timer.current)
          timer.current = window.setTimeout(() => setShow(true), 300)
        }
      }}
    >
      {children}
      {show &&
        createPortal(
          <div
            ref={tipRef}
            className={`fixed ${PAD_TIP} bg-bg-tertiary text-text-primary ${TEXT.xs} ${RADIUS.md} shadow-lg whitespace-nowrap pointer-events-none z-[9999]`}
            style={{
              left: pos.left,
              top: pos.top,
              opacity: ready ? 1 : 0,
            }}
            data-placement={pos.placement}
          >
            {text}
          </div>,
          document.body,
        )}
    </div>
  )
}

// ── ActionBtn: golden-ratio modular scale (×√φ ≈ 1.272), tokens from design.ts ──
export function ActionBtn({
  icon,
  label,
  title,
  variant,
  size,
  onClick,
  className,
}: {
  icon: ReactNode
  label: string
  title: string
  variant: 'primary' | 'danger' | 'outline' | 'outline-accent'
  size?: 'xs' | 'sm' | 'md' | 'lg' | 'xl'
  onClick?: () => void
  className?: string
}) {
  const w = BTN_SIZE_CLASS[size ?? btnAutoSize(label)]
  return (
    <Tooltip text={title}>
      <button
        onClick={onClick}
        className={`inline-flex items-center justify-center gap-1.5 rounded-md px-2.5 ${H.control} text-xs font-medium whitespace-nowrap transition-all duration-150 ${w} ${className ?? ''} ${
          variant === 'primary'
            ? 'bg-accent text-white hover:bg-accent-hover'
            : variant === 'danger'
              ? 'bg-error/20 text-error hover:bg-error/30'
              : variant === 'outline-accent'
                ? 'border border-accent text-accent hover:bg-accent/10'
                : 'border border-border text-text-secondary hover:bg-bg-hover'
        }`}
      >
        {icon}
        <span>{label}</span>
      </button>
    </Tooltip>
  )
}

// ── ThemeBtn ──
export function ThemeBtn({ dark, onToggle }: { dark: boolean; onToggle: () => void }) {
  const { t } = useTranslation()
  return (
    <Tooltip text={dark ? t('theme.switch_light') : t('theme.switch_dark')}>
      <button
        onClick={onToggle}
        className="p-2 rounded-md hover:bg-bg-hover transition-colors"
      >
        {dark ? (
          <Sun className="w-4 h-4 text-text-secondary" />
        ) : (
          <Moon className="w-4 h-4 text-text-secondary" />
        )}
      </button>
    </Tooltip>
  )
}
