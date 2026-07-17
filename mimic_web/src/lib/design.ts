// ═══ Design tokens — single source of truth for UI sizing/spacing/typography ═══
// Every component references constants here; no magic Tailwind values in .tsx files.
//
// Principles:
//   - Heights: golden-ratio modular scale (×√φ ≈ 1.272), baseline h=28px (h-7)
//   - Widths: ActionBtn → SIZE_CLASS; Modal → MODAL_W_*
//   - Spacing: 4px granularity (p-1=4px, p-2=8px, p-3=12px, p-4=16px, p-5=20px)
//   - Typography: 5 tiers (micro/tiny/xs/sm/base), all in px
//   - Border radius: 3 tiers (md/lg/xl)

// ── Heights (h-* → px) ──
export const H = {
  /** Button, input, select — 28px */
  control: 'h-7',
  /** Progress strip, toolbar section */
  strip: 'h-[52px]',
  /** Icon — small (button interior) */
  iconSm: 'w-3.5 h-3.5',
  /** Icon — standard (header, standalone) */
  icon: 'w-4 h-4',
  /** Icon — large (hero) */
  iconLg: 'w-5 h-5',
  /** Icon — xlarge (checking spinner) */
  iconXl: 'w-10 h-10',
  /** Icon — 2xl (success check) */
  icon2xl: 'w-12 h-12',
} as const

// ── ActionBtn widths (golden-ratio scale, all h-7) ──
// Auto-size uses visual width units: Latin=1, CJK/fullwidth≈2 (中文约两倍宽).
export const BTN_SIZE_CLASS: Record<string, string> = {
  xs: 'w-16',        // 64px  — ≤3 units
  sm: 'w-20',        // 80px  — 4–6 units
  md: 'w-[104px]',   // 104px — 7 units
  lg: 'w-[132px]',   // 132px — 8–14 units (e.g. 检查更新=8)
  xl: 'w-[168px]',   // 168px — 15+ units
}

/** Approximate display width: CJK / fullwidth count as 2 Latin units. */
export function btnLabelUnits(label: string): number {
  let n = 0
  for (const ch of label) {
    n += /[\u2e80-\u9fff\uf900-\ufaff\ufe30-\ufe4f\uff00-\uffef]/.test(ch) ? 2 : 1
  }
  return n
}

export function btnAutoSize(label: string): string {
  const n = btnLabelUnits(label)
  if (n <= 3) return 'xs'
  if (n <= 6) return 'sm'
  if (n <= 7) return 'md'
  if (n <= 14) return 'lg'
  return 'xl'
}

// ── Modal widths / heights ──
export const MODAL_W = {
  /** UpdateModal, TargetPickerModal */
  picker: 'w-[520px]',
  /** SelfTestModal */
  test: 'w-[540px]',
} as const

/** Shared modal height — fixed across all UpdateModal statuses (no jump). */
export const MODAL_H = 'h-[min(560px,85vh)] max-h-[min(560px,85vh)]' as const

// ── Spacing (gap-*, p-*, px-*, py-*) ──
export const GAP = {
  xs: 'gap-1',       // 4px  — tight button group
  sm: 'gap-1.5',     // 6px  — button interior icon+text
  md: 'gap-2',       // 8px  — button row, header items
  lg: 'gap-2.5',     // 10px — header icon+title
  xl: 'gap-3',       // 12px — section items
} as const

export const PAD = {
  xs: 'p-1',         // 4px
  sm: 'p-1.5',       // 6px
  md: 'p-2',         // 8px
  lg: 'p-3',         // 12px — card body
  xl: 'p-4',         // 16px
  xxl: 'p-5',        // 20px — modal body
} as const

export const PAD_X = {
  sm: 'px-3',        // modal header/footer horizontal
  md: 'px-4',        // modal header horizontal
  lg: 'px-5',        // modal body horizontal
} as const

export const PAD_Y = {
  sm: 'py-2',        // compact row
  md: 'py-2.5',      // diff header row
  lg: 'py-3',        // modal footer
  xl: 'py-4',        // modal header
} as const

// ── Typography (font-size + line-height) ──
export const TEXT = {
  micro: 'text-[9px]',
  tiny: 'text-[10px]',
  /** Small-mono — file paths, code */
  smallMono: 'text-[11px]',
  /** Standard — labels, hints, button text */
  xs: 'text-xs',
  /** Body — titles, body text */
  sm: 'text-sm',
} as const

// ── Border radius ──
export const RADIUS = {
  md: 'rounded-md',     // buttons, inputs
  lg: 'rounded-lg',     // cards, modals, larger buttons
  xl: 'rounded-xl',     // outer card shells
  full: 'rounded-full', // badges, progress bars
} as const

// ── Misc ──
export const RING = 'ring-1 ring-inset ring-border'

// ── Component presets ──
/** Modal card shell (UpdateModal) — fixed width+height so status changes don't jump */
export const MODAL_CARD = `${MODAL_W.picker} ${MODAL_H} bg-bg-primary rounded-xl ${RING} flex flex-col shadow-2xl overflow-hidden`

/** Collapsible diff container */
export const DIFF_CONTAINER = `bg-bg-secondary rounded-lg overflow-hidden`

/** Diff file-row fixed widths */
export const DIFF_COL = {
  /** Friendly name badge */
  role: 'w-14',
  /** Size number column (解压/流量) */
  num: 'w-20',
  /** Chevron gutter */
  chevron: 'w-5',
} as const
