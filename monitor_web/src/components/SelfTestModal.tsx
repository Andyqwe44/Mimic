// ═══ Self-Test report modal — mapping calibration results ═══
import { useTranslation } from 'react-i18next'
import { X, Crosshair, AlertTriangle } from 'lucide-react'
import type { SelfTestState, SelfTestSummary } from '../lib/selftest'
import { useScrollLock } from '../lib/useScrollLock'
import { Tooltip } from './Toolkit'

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
          <Tooltip
            key={`${x}-${y}`}
            text={`cell[${x},${y}] ${Math.round(rate * 100)}% (${cellCounts[y][x]} samples)`}
          >
            <div
              className="w-11 h-11 rounded flex flex-col items-center justify-center text-[10px] font-mono"
              style={{ background: rateColor(rate), color: rate > 0.5 ? '#0b1220' : '#fff' }}
            >
              <span className="font-bold">{Math.round(rate * 100)}</span>
              <span className="opacity-70">{x},{y}</span>
            </div>
          </Tooltip>
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
  const { t } = useTranslation()

  if (state.phase === 'idle') return null

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/50 backdrop-blur-sm">
      <div className="w-[540px] max-w-[92vw] max-h-[88vh] overflow-y-auto bg-bg-secondary rounded-2xl ring-1 ring-inset ring-border shadow-2xl p-5">
        {/* Header */}
        <div className="flex items-center gap-2 mb-4">
          <Crosshair className="w-4 h-4 text-accent" />
          <span className="text-sm font-semibold text-text-primary">{t('selftest.title')}</span>
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
              {t('selftest.running', {
                done: state.done,
                total: state.total || '?',
                step: state.step === 'scenarios'
                  ? t('selftest.step_scenarios')
                  : t('selftest.step_grid'),
              })}
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
                {t('selftest.abort')}
              </button>
            </div>
          </div>
        )}

        {/* Error */}
        {state.phase === 'error' && (
          <div className="space-y-3">
            <div className="flex items-start gap-2 text-sm text-error">
              <AlertTriangle className="w-4 h-4 mt-0.5 shrink-0" />
              <span>{t('selftest.error_prefix', { error: state.error })}</span>
            </div>
            <div className="flex justify-end">
              <button
                onClick={onClose}
                className="px-3 h-8 rounded-md text-xs font-medium border border-border text-text-secondary hover:bg-bg-hover transition-colors"
              >
                {t('selftest.close')}
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
                <div className="text-xs text-accent-secondary">{t('selftest.aborted_note')}</div>
              )}
              {/* Stats */}
              <div className="grid grid-cols-3 gap-2">
                <Stat label={t('selftest.stat_samples')} value={`${s.total}`} />
                <Stat
                  label={t('selftest.stat_received')}
                  value={`${s.received} (${pct(s.received, s.total)})`}
                  tone={s.received === s.total ? 'text-success' : 'text-error'}
                />
                <Stat
                  label={t('selftest.stat_cell_match')}
                  value={pct(s.cellMatch, s.total)}
                  tone={s.cellMatch === s.total ? 'text-success' : 'text-accent-secondary'}
                />
                <Stat label={t('selftest.stat_hit_match')} value={pct(s.hitMatch, s.total)} />
                <Stat
                  label={t('selftest.stat_offset')}
                  value={`(${s.meanDx.toFixed(1)}, ${s.meanDy.toFixed(1)})`}
                  tone={offsetTone}
                />
                <Stat
                  label={t('selftest.stat_error')}
                  value={`${s.meanAbs.toFixed(1)}/${s.maxAbs.toFixed(1)}`}
                  tone={offsetTone}
                />
              </div>

              {/* Heatmap */}
              <div>
                <div className="text-xs text-text-muted mb-2">
                  {t('selftest.heatmap_title')}
                </div>
                <Heatmap summary={s} />
              </div>

              {/* Interaction scenarios */}
              {s.scenarios && s.scenarios.length > 0 && (
                <div>
                  <div className="text-xs text-text-muted mb-2">
                    {t('selftest.scenarios_title')}
                  </div>
                  <div className="space-y-1">
                    {s.scenarios.map((sc) => (
                      <div
                        key={sc.id}
                        className="flex items-center gap-2 text-xs px-2 py-1.5 rounded-md bg-bg-primary ring-1 ring-inset ring-border"
                      >
                        <span className={sc.ok ? 'text-success' : 'text-error'}>
                          {sc.ok ? '✓' : '✗'}
                        </span>
                        <span className="text-text-primary font-medium w-24 shrink-0">{sc.label}</span>
                        <span className="text-text-muted truncate">{sc.detail}</span>
                      </div>
                    ))}
                  </div>
                </div>
              )}

              {/* Diagnosis hint */}
              <div className="text-[11px] text-text-muted leading-relaxed border-t border-border pt-3">
                {s.received < s.total && <div>{t('selftest.diag_missed')}</div>}
                {s.meanAbs > 4 && <div>{t('selftest.diag_large_error')}</div>}
                {Math.abs(s.meanDx) > 3 || Math.abs(s.meanDy) > 3 ? (
                  <div>{t('selftest.diag_systematic', { dx: s.meanDx.toFixed(1), dy: s.meanDy.toFixed(1) })}</div>
                ) : null}
                {s.scenarios?.some((x) => !x.ok) && (
                  <div>{t('selftest.diag_scenario_fail')}</div>
                )}
                {s.received === s.total && s.cellMatch === s.total && s.meanAbs <= 1.5
                  && (!s.scenarios?.length || s.scenarios.every((x) => x.ok)) && (
                  <div className="text-success">{t('selftest.diag_perfect')}</div>
                )}
              </div>

              <div className="flex justify-end">
                <button
                  onClick={onClose}
                  className="px-3 h-8 rounded-md text-xs font-medium border border-border text-text-secondary hover:bg-bg-hover transition-colors"
                >
                  {t('selftest.close')}
                </button>
              </div>
            </div>
          )
        })()}
      </div>
    </div>
  )
}
