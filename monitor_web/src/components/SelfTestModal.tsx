// ═══ Self-Test report modal — mapping calibration results ═══
import { X, Crosshair, AlertTriangle } from 'lucide-react'
import type { SelfTestState, SelfTestSummary } from '../lib/selftest'
import { useScrollLock } from '../lib/useScrollLock'

// rate 0..1 → red→amber→green
function rateColor(r: number): string {
  const R = Math.round(239 + (34 - 239) * r)
  const G = Math.round(68 + (197 - 68) * r)
  const B = Math.round(68 + (94 - 68) * r)
  return `rgb(${R},${G},${B})`
}

function pct(n: number, d: number): string {
  return d > 0 ? `${Math.round((n / d) * 100)}%` : '—'
}

function Stat({ label, value, tone }: { label: string; value: string; tone?: string }) {
  return (
    <div className="flex flex-col gap-0.5 px-3 py-2 rounded-lg bg-bg-primary ring-1 ring-inset ring-border">
      <span className="text-[10px] uppercase tracking-wide text-text-muted">{label}</span>
      <span className={`text-sm font-mono font-medium ${tone || 'text-text-primary'}`}>{value}</span>
    </div>
  )
}

function Heatmap({ summary }: { summary: SelfTestSummary }) {
  const { cells, cellCounts } = summary
  return (
    <div className="inline-grid gap-1" style={{ gridTemplateColumns: `repeat(${cells.length}, 1fr)` }}>
      {cells.map((row, y) =>
        row.map((rate, x) => (
          <div
            key={`${x}-${y}`}
            className="w-11 h-11 rounded flex flex-col items-center justify-center text-[10px] font-mono"
            style={{ background: rateColor(rate), color: rate > 0.5 ? '#0b1220' : '#fff' }}
            title={`cell[${x},${y}] ${Math.round(rate * 100)}% (${cellCounts[y][x]} samples)`}
          >
            <span className="font-bold">{Math.round(rate * 100)}</span>
            <span className="opacity-70">{x},{y}</span>
          </div>
        )),
      )}
    </div>
  )
}

export function SelfTestModal({
  state,
  onClose,
  onAbort,
}: {
  state: SelfTestState
  onClose: () => void
  onAbort: () => void
}) {
  // Lock body scroll while modal is visible (must be before early return)
  const modalActive = state.phase !== 'idle'
  useScrollLock(modalActive)

  if (state.phase === 'idle') return null

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/50 backdrop-blur-sm">
      <div className="w-[540px] max-w-[92vw] max-h-[88vh] overflow-y-auto bg-bg-secondary rounded-2xl ring-1 ring-inset ring-border shadow-2xl p-5">
        {/* Header */}
        <div className="flex items-center gap-2 mb-4">
          <Crosshair className="w-4 h-4 text-accent" />
          <span className="text-sm font-semibold text-text-primary">映射自检 · Self-Test</span>
          <span className="flex-1" />
          {state.phase !== 'running' && (
            <button
              onClick={onClose}
              className="p-1 rounded-md text-text-muted hover:text-text-primary hover:bg-bg-hover transition-colors"
            >
              <X className="w-4 h-4" />
            </button>
          )}
        </div>

        {/* Running */}
        {state.phase === 'running' && (
          <div className="space-y-3">
            <div className="text-sm text-text-secondary">
              正在扫描点击并比对反馈… {state.done}/{state.total || '?'}
            </div>
            <div className="h-2 rounded-full bg-bg-tertiary overflow-hidden">
              <div
                className="h-full bg-accent transition-all duration-100"
                style={{ width: state.total ? `${(state.done / state.total) * 100}%` : '10%' }}
              />
            </div>
            <div className="flex justify-end">
              <button
                onClick={onAbort}
                className="px-3 h-8 rounded-md text-xs font-medium border border-error/40 text-error hover:bg-error/10 transition-colors"
              >
                中止
              </button>
            </div>
          </div>
        )}

        {/* Error */}
        {state.phase === 'error' && (
          <div className="space-y-3">
            <div className="flex items-start gap-2 text-sm text-error">
              <AlertTriangle className="w-4 h-4 mt-0.5 shrink-0" />
              <span>自检失败：{state.error}</span>
            </div>
            <div className="flex justify-end">
              <button
                onClick={onClose}
                className="px-3 h-8 rounded-md text-xs font-medium border border-border text-text-secondary hover:bg-bg-hover transition-colors"
              >
                关闭
              </button>
            </div>
          </div>
        )}

        {/* Done */}
        {state.phase === 'done' && (() => {
          const s = state.summary
          const offsetTone = s.meanAbs > 4 ? 'text-error' : s.meanAbs > 1.5 ? 'text-accent-secondary' : 'text-success'
          return (
            <div className="space-y-4">
              {s.aborted && (
                <div className="text-xs text-accent-secondary">⚠ 已中止 — 结果为部分样本</div>
              )}
              {/* Stats */}
              <div className="grid grid-cols-3 gap-2">
                <Stat label="样本" value={`${s.total}`} />
                <Stat
                  label="收到反馈"
                  value={`${s.received} (${pct(s.received, s.total)})`}
                  tone={s.received === s.total ? 'text-success' : 'text-error'}
                />
                <Stat
                  label="格子命中匹配"
                  value={pct(s.cellMatch, s.total)}
                  tone={s.cellMatch === s.total ? 'text-success' : 'text-accent-secondary'}
                />
                <Stat label="HIT/MISS 匹配" value={pct(s.hitMatch, s.total)} />
                <Stat
                  label="偏移向量 px"
                  value={`(${s.meanDx.toFixed(1)}, ${s.meanDy.toFixed(1)})`}
                  tone={offsetTone}
                />
                <Stat
                  label="误差 均值/最大"
                  value={`${s.meanAbs.toFixed(1)}/${s.maxAbs.toFixed(1)}`}
                  tone={offsetTone}
                />
              </div>

              {/* Heatmap */}
              <div>
                <div className="text-xs text-text-muted mb-2">
                  格子命中率热力图（预期格 vs 实收格）
                </div>
                <Heatmap summary={s} />
              </div>

              {/* Diagnosis hint */}
              <div className="text-[11px] text-text-muted leading-relaxed border-t border-border pt-3">
                {s.received < s.total && <div>• 有未收到反馈的点 → 输入转发链路可能中断（检查鼠标模式/目标窗口）</div>}
                {s.meanAbs > 4 && <div>• 平均像素误差偏大 → 可能存在坐标偏移/缩放/DPI 不匹配</div>}
                {Math.abs(s.meanDx) > 3 || Math.abs(s.meanDy) > 3 ? (
                  <div>• 存在系统性偏移 ({s.meanDx.toFixed(1)},{s.meanDy.toFixed(1)})px → 常量偏移，建议校准原点</div>
                ) : null}
                {s.received === s.total && s.cellMatch === s.total && s.meanAbs <= 1.5 && (
                  <div className="text-success">• 映射精确，全部命中预期格，像素误差在取整范围内 ✓</div>
                )}
              </div>

              <div className="flex justify-end">
                <button
                  onClick={onClose}
                  className="px-3 h-8 rounded-md text-xs font-medium border border-border text-text-secondary hover:bg-bg-hover transition-colors"
                >
                  关闭
                </button>
              </div>
            </div>
          )
        })()}
      </div>
    </div>
  )
}
