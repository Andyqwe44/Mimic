// ═══ LoadingScreen — startup skeleton screen ═══
// Mirrors the real default screen (Settings tab): top bar + left settings column
// (StatusBar strip + collapsible cards) + resize divider + right panel stack +
// bottom status bar. Content is replaced by shimmering placeholder blocks,
// Alipay-style. Shown as an overlay right after the host reveals the (initially
// hidden) window, then swapped for the real UI — the window never appears white.
//
// This is a hand-authored approximation of the layout, NOT derived from the real
// components — keep it roughly in sync if the shell layout changes materially.
//
// NOTE (deferred idea): per-widget skeletons for the genuinely async parts of the
// first view (log history, window list, persisted settings) were considered but
// shelved — those loads are millisecond-scale, not worth the complexity yet.

// One shimmering placeholder block. `className` sizes/positions it (Tailwind);
// the `.skeleton` class supplies the base color + sweep animation (index.css).
function Bar({ className }: { className: string }) {
  return <div className={`skeleton ${className}`} />
}

// A settings-style card: rounded panel with a header (icon + title / chevron) and
// a body of placeholder rows. `rows` varies the body height between cards.
function Card({ rows = 3 }: { rows?: number }) {
  return (
    <div className="overflow-hidden rounded-xl bg-bg-secondary ring-1 ring-inset ring-border">
      <div className="flex items-center justify-between px-4 py-3">
        <div className="flex items-center gap-2">
          <Bar className="h-4 w-4" />
          <Bar className="h-4 w-24" />
        </div>
        <Bar className="h-4 w-4" />
      </div>
      <div className="space-y-2.5 border-t border-border p-4">
        {Array.from({ length: rows }).map((_, i) => (
          <Bar key={i} className={`h-8 ${i % 3 === 2 ? 'w-2/3' : 'w-full'}`} />
        ))}
      </div>
    </div>
  )
}

export function LoadingScreen() {
  return (
    <div className="absolute inset-0 z-50 flex flex-col bg-bg-primary select-none">
      {/* Top bar — tabs (left) + Start / theme (right) */}
      <div className="flex h-10 shrink-0 items-center border-b border-border bg-bg-secondary">
        <div className="flex h-full flex-1 items-center">
          {[0, 1, 2].map((i) => (
            <div
              key={i}
              className="flex h-full min-w-[100px] items-center gap-1.5 border-r border-border px-3"
            >
              <Bar className="h-3.5 w-3.5" />
              <Bar className="h-3.5 w-14" />
            </div>
          ))}
        </div>
        <div className="flex items-center gap-1 px-2">
          <Bar className="h-7 w-20" />
          <div className="mx-1 h-4 w-px bg-border" />
          <Bar className="h-7 w-7" />
          <Bar className="h-7 w-7" />
          <Bar className="h-7 w-7" />
        </div>
      </div>

      {/* Body — left settings column + divider + right panel stack */}
      <div className="flex flex-1 overflow-hidden">
        {/* Left: settings column */}
        <div className="flex-1 space-y-3 overflow-hidden p-6" style={{ minWidth: 360 }}>
          {/* StatusBar strip */}
          <div className="flex items-center gap-4 rounded-xl bg-bg-secondary px-4 py-2.5 ring-1 ring-inset ring-border">
            <Bar className="h-3.5 w-24" />
            <Bar className="h-3.5 w-20" />
            <Bar className="h-3.5 w-16" />
            <div className="flex-1" />
            <Bar className="h-3.5 w-28" />
          </div>
          {/* Connection / Capture / Model / General / About */}
          <Card rows={3} />
          <Card rows={6} />
          <Card rows={2} />
          <Card rows={5} />
          <Card rows={2} />
        </div>

        {/* Resize divider */}
        <div className="flex w-1 shrink-0 items-center justify-center">
          <div className="h-8 w-[2px] rounded-full bg-border" />
        </div>

        {/* Right: panel stack (Connection / Screenshot / Log) */}
        <div className="flex shrink-0 flex-col gap-3 p-3" style={{ width: 324 }}>
          <Card rows={3} />
          <Card rows={2} />
          <Card rows={4} />
        </div>
      </div>

      {/* Bottom status bar */}
      <div className="flex h-9 shrink-0 items-center gap-3 border-t border-border bg-bg-secondary px-4">
        <Bar className="h-3 w-24" />
        <Bar className="h-3 w-16" />
        <Bar className="h-3 w-20" />
        <div className="flex-1" />
        <Bar className="h-3 w-12" />
      </div>
    </div>
  )
}
