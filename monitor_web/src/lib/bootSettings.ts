// ═══ Boot settings — applied before React first paint ═══
// C++ injects window.__BOOT_SETTINGS__ via AddScriptToExecuteOnDocumentCreated
// (and applies theme CSS in the same script). This module lets React init state
// from that snapshot so accents/theme never flash defaults → saved values.

export type BootSettings = {
  theme?: string
  mouseMode?: string
  keyMode?: string
  mappingHotkey?: string
  devMode?: boolean
  selfTargetMode?: string
  keepFiles?: number
  autoSnap?: boolean
  autoStream?: boolean
  snapMethod?: string
  streamMethod?: string
  renderMethod?: string
  normalAccent?: string
  normalSecondaryAccent?: string
  devAccent?: string
  devSecondaryAccent?: string
  locale?: string
}

declare global {
  interface Window {
    __BOOT_SETTINGS__?: BootSettings
  }
}

export function readBootSettings(): BootSettings {
  const s = window.__BOOT_SETTINGS__
  if (s && typeof s === 'object' && !Array.isArray(s)) return s
  return {}
}

export function hasBootSettings(): boolean {
  return window.__BOOT_SETTINGS__ !== undefined
}

function darkenHex(hex: string, pct: number): string {
  if (!hex || hex[0] !== '#' || hex.length < 7) return hex
  const v = parseInt(hex.slice(1), 16)
  if (Number.isNaN(v)) return hex
  const r = Math.max(0, ((v >> 16) & 0xff) - Math.round(255 * pct / 100))
  const g = Math.max(0, ((v >> 8) & 0xff) - Math.round(255 * pct / 100))
  const b = Math.max(0, (v & 0xff) - Math.round(255 * pct / 100))
  return `#${((r << 16) | (g << 8) | b).toString(16).padStart(6, '0')}`
}

/** Apply theme class + accent CSS vars from boot snapshot (idempotent). */
export function applyBootTheme(s: BootSettings = readBootSettings()) {
  const theme = s.theme === 'light' || s.theme === 'dark' || s.theme === 'system' ? s.theme : 'light'
  const systemDark = window.matchMedia('(prefers-color-scheme: dark)').matches
  const dark = theme === 'dark' || (theme === 'system' && systemDark)
  document.documentElement.classList.toggle('dark', dark)

  const useDev = !!s.devMode
  const accent = (useDev ? s.devAccent : s.normalAccent) || '#3B82F6'
  const secondary = (useDev ? s.devSecondaryAccent : s.normalSecondaryAccent) || '#F97316'
  document.documentElement.style.setProperty('--color-accent', accent)
  document.documentElement.style.setProperty('--color-accent-secondary', secondary)
  document.documentElement.style.setProperty('--color-accent-hover', darkenHex(accent, 15))
}
