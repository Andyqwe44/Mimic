// ═══ LoadingScreen — startup skeleton matching unified shell ═══

function Bar({ className }: { className: string }) {
  return <div className={`skeleton ${className}`} />
}

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
    <div className="absolute inset-0 z-50 flex bg-bg-primary select-none max-[599px]:flex-col">
      {/* Side nav skeleton — desktop/tablet */}
      <div className="hidden min-[600px]:flex w-20 min-[600px]:max-[959px]:w-14 shrink-0 flex-col border-r border-border bg-bg-secondary p-2 gap-2">
        <Bar className="h-4 w-12 mx-auto mt-2" />
        {[0, 1, 2, 3].map((i) => (
          <Bar key={i} className="h-10 w-full rounded-lg" />
        ))}
        <div className="flex-1" />
        <Bar className="h-3 w-10 mx-auto mb-2" />
      </div>

      <div className="flex-1 flex flex-col min-w-0">
        <div className="flex h-11 shrink-0 items-center gap-2 border-b border-border bg-bg-secondary px-3">
          <Bar className="h-4 w-16" />
          <div className="flex-1" />
          <Bar className="h-8 w-28 rounded-full" />
        </div>
        <div className="flex-1 space-y-3 overflow-hidden p-4 max-[359px]:p-2">
          <Card rows={2} />
          <Card rows={4} />
          <Card rows={3} />
        </div>
        {/* Bottom nav skeleton — phone */}
        <div className="flex min-[600px]:hidden h-14 shrink-0 items-center justify-around border-t border-border bg-bg-secondary px-2">
          {[0, 1, 2, 3].map((i) => (
            <div key={i} className="flex flex-col items-center gap-1">
              <Bar className="h-4 w-4" />
              <Bar className="h-2 w-8" />
            </div>
          ))}
        </div>
      </div>
    </div>
  )
}
