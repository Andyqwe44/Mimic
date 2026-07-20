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
  /** Grid / stacked rows — fill the cell (Shizuku card etc.) */
  fill: 'w-full',
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
  /** UpdateModal, TargetPickerModal — shrink on phone */
  picker: 'w-[min(520px,92vw)]',
  /** SelfTestModal */
  test: 'w-[min(540px,92vw)]',
} as const

/** Shared modal height — fixed across all UpdateModal statuses (no jump). */
export const MODAL_H = 'h-[min(560px,85vh)] max-h-[min(560px,85vh)] max-[479px]:h-[min(100dvh,100%)] max-[479px]:max-h-[100dvh]' as const

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

// ── Responsive shell (viewport-only; PC narrow = phone) ──
export const BP = {
  /** Side nav (icon+label) */
  desktop: 960,
  /** Side icon nav → bottom nav */
  tablet: 600,
  /** Ultra-narrow density */
  narrow: 360,
  /** Short viewport → compress header / sheet modals */
  short: 480,
} as const

export const NAV = {
  sideWide: 'w-20',       // 80px — desktop side rail
  sideCompact: 'w-14',    // 56px — tablet icon rail
  /** Bottom tab bar content height (safe-area applied outside) */
  bottomH: 'h-14',        // 56px
  touchMin: 'min-h-11',   // 44px hit area (no gray press chrome)
  /** Sliding focus pill — slightly taller + lower than before */
  pillH: 'h-10',          // 40px
  pillTop: 'top-2',       // 8px from content top
  /** Thin accent ring — rgba token (not opacity color-mix) */
  pillRing: 'ring-[0.5px] ring-inset ring-accent-ring',
  /** Soft fill for sliding focus pill — rgba token */
  pillBg: 'bg-accent-soft',
  /** Shared settle duration for page track + nav pill (ms) — legacy single-step */
  settleMs: 300,
  settleEase: 'cubic-bezier(0.25, 0.85, 0.3, 1)',
  /**
   * Unified page transition (finger settle + nav tap) — same curve & duration.
   * Approximates the old native scroll-snap settle feel (ViewPager2-ish ease-out).
   * Distance only changes peak speed; wall-clock time stays fixed.
   */
  pageAnimMs: 220,
  /** cubic-bezier — snappy ease-out (was pagerSnapEase) */
  pageAnimEase: [0.22, 0.9, 0.28, 1] as const,
  /** @deprecated alias — use pageAnimMs */
  tapDurMs: 220,
  /** @deprecated alias — use pageAnimEase */
  tapEase: [0.22, 0.9, 0.28, 1] as const,
  /** grid gap-1 between bottom tabs */
  bottomGap: 'gap-1',
  bottomGapRem: 0.25,
  /**
   * Paged horizontal track (Clash Royale / ViewPager2–style).
   * No OS API in WebView — axis lock mirrors Android: after touchSlop, dominate
   * axis wins; ties → H (pager owns horizontal). See resolvePagerAxis().
   */
  /** ~8px ≈ ViewConfiguration touchSlop at 1x; slightly diagonal still locks H */
  pagerAxisLockPx: 8,
  /**
   * H cone: lock H when adx >= ady * cone (1.0 = 45°).
   * <1 prefers H on mild diagonals (抖音/B站 nested H-pager feel).
   */
  pagerHCone: 0.85,
  pagerRubber: 0.38,
  pagerRubberMax: 0.28,
  /** ~15% width to commit by distance */
  pagerSnapThreshold: 0.15,
  /** Fling must be clearly fast AND already moved a real distance (see minDelta/minMs) */
  pagerFlingPagesPerMs: 0.0022,
  pagerFlingStaleMs: 60,
  pagerFlingMinDelta: 0.12,
  pagerFlingMinMs: 80,
  pagerReverseCancel: 0.12,
  /** @deprecated alias — use pageAnimMs */
  pagerSnapMs: 220,
  /** @deprecated alias — use pageAnimEase */
  pagerSnapEase: [0.22, 0.9, 0.28, 1] as const,
  /** Android long-press tooltip (PC keeps hover 300ms) */
  tooltipHoverMs: 300,
  tooltipLongPressMs: 450,
} as const

/**
 * ViewPager2-style axis resolve after touch slop.
 * Returns 'none' until slop; then H if within horizontal cone, else V.
 */
export function resolvePagerAxis(
  adx: number,
  ady: number,
  slop = NAV.pagerAxisLockPx,
  hCone = NAV.pagerHCone,
): 'none' | 'h' | 'v' {
  if (adx < slop && ady < slop) return 'none'
  // Mild diagonal → H (adx >= ady * 0.85 ≈ up to ~40° from horizontal)
  if (adx >= ady * hCone) return 'h'
  return 'v'
}

/** Fixed page-anim duration (ms) — distance only changes peak velocity, not time. */
export function navTapDurationMs(_pageDelta?: number): number {
  return NAV.pageAnimMs
}

/**
 * Rubber-band a fractional page position outside [0, pageCount-1].
 * Extensible: pageCount = PRIMARY_PAGES.length (or any N).
 */
export function rubberBandPage(raw: number, pageCount: number): number {
  if (pageCount <= 0) return 0
  const max = pageCount - 1
  if (raw >= 0 && raw <= max) return raw
  const r = NAV.pagerRubber
  const cap = NAV.pagerRubberMax
  if (raw < 0) return -Math.min(cap, -raw * r)
  return max + Math.min(cap, (raw - max) * r)
}

export const SHELL_PAD = {
  /** Match Log panel density — full-bleed content width, generous padding */
  page: 'p-6 max-[359px]:p-3',
  pageY: 'py-6 max-[359px]:py-3',
  safeBottom: 'pb-[env(safe-area-inset-bottom,0px)]',
  safeTop: 'pt-[env(safe-area-inset-top,0px)]',
} as const

// ── Soft fills (Android-safe rgba theme colors — never use accent/N opacity mods) ──
export const SOFT = {
  accentBg: 'bg-accent-soft',
  accentBgMid: 'bg-accent-soft-mid',
  accentBgStrong: 'bg-accent-soft-strong',
  accentRing: 'ring-accent-ring',
  accentBorder: 'border-accent-ring',
  secondaryBg: 'bg-accent-secondary-soft',
  secondaryBgMid: 'bg-accent-secondary-soft-mid',
  secondaryRing: 'ring-accent-secondary-ring',
  secondaryBorder: 'border-accent-secondary-ring',
  successBg: 'bg-success-soft',
  successBgMid: 'bg-success-soft-mid',
  successRing: 'ring-success-ring',
  warnBg: 'bg-warn-soft',
  warnRing: 'ring-warn-ring',
  errorBg: 'bg-error-soft',
  errorBgMid: 'bg-error-soft-mid',
  errorRing: 'ring-error-ring',
  blueBg: 'bg-blue-soft',
  violetBg: 'bg-violet-soft',
  mutedFg: 'text-muted-soft',
  scrim: 'bg-scrim',
} as const

/** Peer Monitor workspace: preview follows remote frame aspect (portrait/landscape). */
export const PEER_WORKSPACE = {
  /** Outer slot — grows with content aspect; portrait gets more vertical room. */
  previewWeight: 'shrink-0 flex flex-col min-h-0 w-full',
  panelWeight: 'flex-1 min-h-0',
} as const

/** Floating UU-style virtual mouse panel — width hugs buttons (no trailing blank). */
export const VMOUSE = {
  panel: 'w-max',
  btnW: 'w-12',
  btnH: 'h-8',
  wheelW: 'w-8',
  handleH: 'h-5',
  /** Outer black stroke so panel stays visible on light/dark remote frames. */
  stroke: 'shadow-[0_0_0_1.5px_#000]',
  pad: 'p-1',
} as const

/** In-app soft keyboard (peer remote control). */
export const SOFT_KB = {
  keyH: 'h-11',
  keyHLg: 'h-12',
  gap: 'gap-1.5',
  pad: 'p-2.5',
  spaceY: 'space-y-2',
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
