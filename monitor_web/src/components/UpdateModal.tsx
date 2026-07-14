// ═══ UpdateModal — check result: update available / already latest / checking / error ═══
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { X, Download, ArrowRight, FileStack, Package, ChevronDown, CheckCircle2, AlertTriangle, Loader2, RotateCcw } from 'lucide-react'
import { ActionBtn } from './Toolkit'
import { addLog, type UpdateProgressMsg } from '../lib/bridge'
import { useScrollLock } from '../lib/useScrollLock'
import { MODAL_CARD, DIFF_CONTAINER, DIFF_COL } from '../lib/design'

// 弹窗状态: 检查中 / 有更新 / 已最新 / 出错. 缺省视为 'update' (向后兼容).
export type UpdateStatus = 'checking' | 'update' | 'latest' | 'error'

export interface UpdateInfo {
  current: string
  latest: string
  name: string
  body: string
  url: string
  status?: UpdateStatus  // 缺省 = 'update'
  error?: string         // status==='error' 时的错误文案
  message?: string      // server-supplied note (manifest "message")
  mandatory?: boolean   // manifest "mandatory" → hide "Later"
  mode?: string         // 'incremental' | 'full'
  // per-file changes. size = 解压后磁盘占用; dl = 下载流量 (压缩后).
  // dl 缺省时 == size (当前逐文件裸下载, 无压缩; 将来压缩下载填 dl).
  diff?: Array<{ path: string; size?: number; dl?: number }>
  // staging state from check_update: partial download from a prior interrupted session
  staging_state?: {
    has_partial: boolean
    done_files: number
    total_files: number
    done_bytes: number
    total_bytes: number
    done_paths: string[]    // file paths already in staging (for strikethrough)
  }
}

// Human-readable byte size.
function fmtSize(b: number): string {
  if (b <= 0) return '0 B'
  if (b < 1024) return `${b} B`
  if (b < 1024 * 1024) return `${(b / 1024).toFixed(1)} KB`
  return `${(b / 1024 / 1024).toFixed(2)} MB`
}

// path → 友好功能名 (徽章). install 根目录相对路径分类.
function fileRole(path: string, t: (key: string) => string): { label: string; core: boolean } {
  const p = path.toLowerCase()
  const base = p.split('/').pop() || p
  if (base === 'monitor_app.exe') return { label: t('update.file_roles.main_exe'), core: true }
  if (base.startsWith('updater')) return { label: t('update.file_roles.updater'), core: true }
  if (base === 'version.json') return { label: t('update.file_roles.manifest'), core: true }
  if (base.startsWith('logger')) return { label: t('update.file_roles.logger'), core: false }
  if (base.startsWith('capture')) return { label: t('update.file_roles.capture'), core: false }
  if (base.startsWith('input')) return { label: t('update.file_roles.input'), core: false }
  if (p.startsWith('frontend/')) return { label: t('update.file_roles.frontend'), core: false }
  if (p.startsWith('config/')) return { label: t('update.file_roles.config'), core: false }
  return { label: t('update.file_roles.file'), core: false }
}

// 下载流量 (dl 优先, 缺省回退到解压大小).
const traffic = (f: { size?: number; dl?: number }) => f.dl ?? f.size ?? 0

// Header 标题 + 图标随状态变.
function headerFor(status: UpdateStatus, t: (key: string) => string) {
  switch (status) {
    case 'checking': return { title: t('update.checking'), icon: <Loader2 className="w-5 h-5 text-accent animate-spin" /> }
    case 'latest':   return { title: t('update.latest'), icon: <CheckCircle2 className="w-5 h-5 text-accent" /> }
    case 'error':    return { title: t('update.error'), icon: <AlertTriangle className="w-5 h-5 text-error" /> }
    default:         return { title: t('update.update_available'), icon: <Download className="w-5 h-5 text-accent" /> }
  }
}

export function UpdateModal({
  info,
  downloading,
  progress,
  onDownload,
  onClearAndDownload,
  onForceUpdate,
  onClose,
}: {
  info: UpdateInfo
  downloading: boolean
  progress?: UpdateProgressMsg | null
  onDownload: () => void
  onClearAndDownload?: () => void
  onForceUpdate?: () => void
  onClose: () => void
}) {
  const [expanded, setExpanded] = useState(false)

  // Lock body scroll while modal is mounted
  useScrollLock()

  const { t } = useTranslation()

  const status: UpdateStatus = info.status ?? 'update'
  const isUpdate = status === 'update'
  const hdr = headerFor(status, t)

  const diff = info.diff || []
  const nFiles = diff.length
  const ss = info.staging_state  // partial download from prior interrupted session
  const hasResume = ss?.has_partial && !downloading
  // Totals for the collapsed header: exclude already-done files when resuming
  const pendingDiff = hasResume && ss?.done_paths ? diff.filter(f => !ss.done_paths!.includes(f.path)) : diff
  const totalSize = pendingDiff.reduce((s, f) => s + (f.size || 0), 0)
  const totalDl = pendingDiff.reduce((s, f) => s + traffic(f), 0)
  const nPending = pendingDiff.length
  const serverFull = info.mode === 'full'  // 服务端/min_version 强制全量

  // mandatory 只对「有更新」态有意义; 其余态一律允许关闭.
  const canClose = !(isUpdate && info.mandatory)

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center">
      {/* Backdrop */}
      <div className="absolute inset-0 bg-black/60" onClick={canClose ? onClose : undefined} />
      {/* Card — 宽固定 520 (= Select 弹窗). 有更新态固定高 min(560,85vh) 防下载时跳;
          检查中/已最新/出错态自适应高 (内容少不留空) */}
      <div
        className={`relative ${MODAL_CARD} ${isUpdate ? 'min-h-[min(560px,85vh)]' : ''}`}
      >
        {/* Header */}
        <div className="flex items-center justify-between px-5 py-4 border-b border-border shrink-0">
          <div className="flex items-center gap-2.5">
            {hdr.icon}
            <span className="font-semibold text-sm text-text-primary">{hdr.title}</span>
          </div>
          {canClose && (
            <button
              onClick={onClose}
              className="w-7 h-7 rounded-lg flex items-center justify-center hover:bg-bg-tertiary transition-colors"
            >
              <X className="w-4 h-4 text-text-secondary" />
            </button>
          )}
        </div>

        {/* ── Non-update states: checking / latest / error (居中简卡) ── */}
        {status === 'checking' && (
          <div className="flex flex-col items-center justify-center gap-3 px-5 py-10 text-center">
            <Loader2 className="w-10 h-10 text-accent animate-spin" />
            <div className="text-sm text-text-primary">{t('update.checking_text')}</div>
            <div className="text-xs text-text-muted font-mono">{t('update.current_version', { version: info.current })}</div>
          </div>
        )}
        {status === 'latest' && (
          <div className="flex flex-col items-center justify-center gap-3 px-5 py-10 text-center">
            <CheckCircle2 className="w-12 h-12 text-accent" />
            <div className="text-sm font-medium text-text-primary">{t('update.latest_text')}</div>
            <span className="px-3 py-1.5 rounded-lg bg-accent/10 ring-1 ring-inset ring-accent/30 text-sm font-mono font-semibold text-accent">
              v{info.current}
            </span>
          </div>
        )}
        {status === 'error' && (
          <div className="flex flex-col items-center justify-center gap-3 px-5 py-10 text-center">
            <AlertTriangle className="w-10 h-10 text-error" />
            <div className="text-sm font-medium text-text-primary">{t('update.error_text')}</div>
            <div className="text-xs text-text-secondary leading-relaxed max-w-[380px]">
              {info.error || t('update.error_fallback')}
            </div>
            <div className="text-xs text-text-muted font-mono">{t('update.current_version', { version: info.current })}</div>
          </div>
        )}

        {/* ── Update state: 完整更新 UI ── */}
        {isUpdate && (
          <div className="flex-1 min-h-0 px-5 py-4 space-y-3 overflow-y-auto">
            {/* Version comparison */}
            <div className="flex items-center justify-center gap-3 py-1">
              <span className="px-3 py-1.5 rounded-lg bg-bg-tertiary text-sm font-mono text-text-muted">
                v{info.current}
              </span>
              <ArrowRight className="w-4 h-4 text-accent" />
              <span className="px-3 py-1.5 rounded-lg bg-accent/10 ring-1 ring-inset ring-accent/30 text-sm font-mono font-semibold text-accent">
                v{info.latest}
              </span>
            </div>

            {/* Server message (manifest "message") */}
            {info.message && (
              <div className="bg-accent/10 ring-1 ring-inset ring-accent/30 rounded-lg p-3 text-xs text-text-primary leading-relaxed">
                {info.message}
              </div>
            )}

            {/* Resume banner — partial download detected in staging */}
            {hasResume && ss && (
              <div className="bg-amber-500/10 ring-1 ring-inset ring-amber-500/30 rounded-lg p-3">
                <div className="flex items-start gap-2.5">
                  <RotateCcw className="w-4 h-4 text-amber-500 mt-0.5 shrink-0" />
                  <div className="flex-1 min-w-0">
                    <div className="text-xs font-medium text-amber-600 dark:text-amber-400 mb-1">
                      {t('update.resume_banner')}
                    </div>
                    <div className="text-xs text-text-secondary leading-relaxed">
                      {t('update.resume_detail', {
                        done: ss.done_files,
                        total: ss.total_files,
                        doneSize: fmtSize(ss.done_bytes),
                        totalSize: fmtSize(ss.total_bytes),
                      })}
                    </div>
                  </div>
                </div>
              </div>
            )}

            {/* Full-package hint */}
            {serverFull && (
              <div className="text-xs text-text-secondary text-center">
                {t('update.full_package_hint')}
              </div>
            )}

            {/* Release name */}
            {info.name && (
              <div className="text-sm font-medium text-text-primary text-center">
                {info.name}
              </div>
            )}

            {/* Changelog */}
            {info.body && (
              <div className="bg-bg-secondary rounded-lg p-3 max-h-40 overflow-y-auto">
                <div className="text-xs text-text-secondary whitespace-pre-wrap font-mono leading-relaxed">
                  {info.body}
                </div>
              </div>
            )}

            {/* ── Collapsible diff (铁律 5: 画=实发). 折叠条右侧两列(解压/流量)与文件行
                   数字列共用固定宽 w-20; chevron 独占 w-5 gutter, 文件行留等宽空槽对齐 ── */}
            {nFiles > 0 && (
              <div className={DIFF_CONTAINER}>
                {/* Header row — whole bar toggles, single-line horizontal */}
                <button
                  onClick={() => setExpanded((v) => !v)}
                  className="w-full flex items-center gap-2 px-3 py-2.5 hover:bg-bg-tertiary/50 transition-colors text-left"
                >
                  <span className="flex-1 min-w-0 text-xs text-text-secondary truncate">
                    {t('update.this_update')} · <span className="font-medium text-text-primary">{t('update.files', { n: nPending })}</span>
                    {hasResume && nPending < nFiles && (
                      <span className="text-text-muted">{t('update.completed_n', { n: nFiles - nPending })}</span>
                    )}
                  </span>
                  {/* 解压总计 — label+number same line */}
                  <span className={`${DIFF_COL.num} shrink-0 text-right text-xs tabular-nums leading-tight`}>
                    <span className="text-text-muted">{t('update.extract')} </span>
                    <span className="font-mono font-medium text-text-primary">{fmtSize(totalSize)}</span>
                  </span>
                  {/* 流量总计 — label+number same line */}
                  <span className={`${DIFF_COL.num} shrink-0 text-right text-xs tabular-nums leading-tight`}>
                    <span className="text-text-muted">{t('update.traffic')} </span>
                    <span className="font-mono font-medium text-accent">{fmtSize(totalDl)}</span>
                  </span>
                  {/* chevron — vertically centered, no caption trick needed */}
                  <span className={`${DIFF_COL.chevron} shrink-0 flex items-center justify-center`}>
                    <ChevronDown
                      className={`w-4 h-4 text-text-muted transition-transform ${expanded ? 'rotate-180' : ''}`}
                    />
                  </span>
                </button>

                {/* Expanded file list */}
                {expanded && (
                  <div className="max-h-44 overflow-y-auto border-t border-border/60 divide-y divide-border/40">
                    {diff.map((f, i) => {
                      const role = fileRole(f.path, t)
                      const isDone = ss?.done_paths?.includes(f.path)
                      return (
                        <div key={i} className={`flex items-center gap-2 px-3 py-1.5 ${isDone ? 'opacity-50' : ''}`}>
                          {/* 友好名徽章 (第一深位, 固定宽 → 路径左对齐) */}
                          <span
                            className={`w-14 shrink-0 text-center text-[10px] py-0.5 rounded ring-1 ring-inset
                              ${role.core
                                ? 'bg-accent/10 text-accent ring-accent/30'
                                : 'bg-bg-tertiary text-text-muted ring-border'}`}
                          >
                            {role.label}
                          </span>
                          {/* 路径 (第二深位, install 根相对, 过长截断). Done → strikethrough */}
                          <span className={`flex-1 min-w-0 truncate text-[11px] font-mono ${isDone ? 'line-through text-text-muted' : 'text-text-secondary'}`}>
                            {f.path}
                          </span>
                          {/* 解压大小 */}
                          <span className={`w-20 shrink-0 text-right text-[11px] font-mono tabular-nums ${isDone ? 'line-through text-text-muted' : 'text-text-muted'}`}>
                            {fmtSize(f.size || 0)}
                          </span>
                          {/* 下载流量 (最右数据列) */}
                          <span className={`w-20 shrink-0 text-right text-[11px] font-mono tabular-nums ${isDone ? 'line-through text-text-muted' : 'text-text-secondary'}`}>
                            {fmtSize(traffic(f))}
                          </span>
                          {/* chevron 等宽空槽 → 与折叠条对齐 */}
                          <span className="w-5 shrink-0" />
                        </div>
                      )
                    })}
                  </div>
                )}
              </div>
            )}
          </div>
        )}

        {/* Progress strip — 仅有更新态渲染; 常驻固定高, idle 空占位, 下载中填充 → 加进度条窗口不跳 */}
        {isUpdate && (
          <div className="h-[52px] shrink-0 px-5 flex flex-col justify-center border-t border-border">
            {downloading && (
              progress ? (
                <div className="space-y-1.5">
                  <div className="text-xs text-text-secondary">
                    {progress.phase === 'download' && (
                      <>{t('update.progress_download', { file: progress.file || '...', current: Math.min(progress.current_file, progress.total_files) || 1, total: progress.total_files })}</>
                    )}
                    {progress.phase === 'done' && <>{t('update.progress_done')}</>}
                    {progress.phase === 'error' && (
                      <span className="text-error">{t('update.progress_error', { file: progress.error_file || progress.file })}</span>
                    )}
                  </div>
                  {progress.phase !== 'error' && (
                    <div className="h-2 rounded-full bg-bg-tertiary overflow-hidden">
                      <div
                        className="h-full bg-accent transition-all duration-100"
                        style={{
                          width: progress.total_bytes > 0
                            ? `${Math.min(100, (progress.done_bytes / progress.total_bytes) * 100)}%`
                            : progress.total_files > 0
                              ? `${Math.min(100, (progress.current_file / progress.total_files) * 100)}%`
                              : '15%',
                        }}
                      />
                    </div>
                  )}
                </div>
              ) : (
                <div className="text-xs text-text-muted text-center animate-pulse">
                  {t('update.progress_preparing')}
                </div>
              )
            )}
          </div>
        )}

        {/* Footer */}
        <div className="flex items-center justify-end gap-2 px-5 py-3 border-t border-border shrink-0">
          {isUpdate ? (
            <>
              {!info.mandatory && (
                <ActionBtn
                  label={t('update.later')}
                  title={t('update.later_tip')}
                  icon={<X className="w-3.5 h-3.5" />}
                  variant="outline"
                  size="sm"
                  onClick={() => {
                    addLog('[update] dismissed')
                    onClose()
                  }}
                />
              )}
              {/* 重新下载 — clear staging then full download. Shown when resumable. */}
              {hasResume && onClearAndDownload && (
                <ActionBtn
                  label={t('update.re_download')}
                  title={t('update.re_download_tip')}
                  icon={<RotateCcw className="w-3.5 h-3.5" />}
                  variant="outline"
                  size="lg"
                  onClick={() => {
                    addLog('[update] clear staging + re-download')
                    onClearAndDownload()
                  }}
                />
              )}
              {/* 全量更新 — 强制重下全部文件. serverFull 时增量按钮即全量, 隐藏此按钮避免重复 */}
              {!hasResume && !downloading && onForceUpdate && !serverFull && (
                <ActionBtn
                  label={t('update.force_update')}
                  title={t('update.force_update_tip')}
                  icon={<Package className="w-3.5 h-3.5" />}
                  variant="outline"
                  size="lg"
                  onClick={() => {
                    addLog('[update] force full update')
                    onForceUpdate()
                  }}
                />
              )}
              {/* 增量更新 / 继续下载 (primary) — auto-skips staging files via sha256 */}
              <ActionBtn
                label={downloading ? t('update.installing') : hasResume ? t('update.continue_download') : serverFull ? t('update.force_update') : t('update.incremental')}
                title={hasResume ? t('update.continue_download_tip') : serverFull ? t('update.force_update_tip') : t('update.incremental_tip')}
                icon={hasResume ? <RotateCcw className="w-3.5 h-3.5" /> : serverFull ? <Package className="w-3.5 h-3.5" /> : <FileStack className="w-3.5 h-3.5" />}
                variant="primary"
                size="lg"
                onClick={() => {
                  addLog(`[update] downloading v${info.latest}`)
                  onDownload()
                }}
              />
            </>
          ) : status === 'checking' ? (
            <ActionBtn
              label={t('common.cancel')}
              title={t('common.cancel')}
              icon={<X className="w-3.5 h-3.5" />}
              variant="outline"
              size="sm"
              onClick={onClose}
            />
          ) : (
            /* latest / error — 单个关闭按钮 */
            <ActionBtn
              label={t('update.got_it')}
              title={t('update.got_it_tip')}
              icon={<CheckCircle2 className="w-3.5 h-3.5" />}
              variant="primary"
              size="sm"
              onClick={onClose}
            />
          )}
        </div>
      </div>
    </div>
  )
}
