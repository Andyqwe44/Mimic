// ═══ UpdateModal — new version available, changelog + download ═══
import { useState } from 'react'
import { X, Download, ArrowRight, FileStack, Package, ChevronDown } from 'lucide-react'
import { ActionBtn } from './Toolkit'
import { addLog, type UpdateProgressMsg } from '../lib/bridge'

export interface UpdateInfo {
  current: string
  latest: string
  name: string
  body: string
  url: string
  message?: string      // server-supplied note (manifest "message")
  mandatory?: boolean   // manifest "mandatory" → hide "Later"
  mode?: string         // 'incremental' | 'full'
  // per-file changes. size = 解压后磁盘占用; dl = 下载流量 (压缩后).
  // dl 缺省时 == size (当前逐文件裸下载, 无压缩; 将来压缩下载填 dl).
  diff?: Array<{ path: string; size?: number; dl?: number }>
}

// Human-readable byte size.
function fmtSize(b: number): string {
  if (b <= 0) return '0 B'
  if (b < 1024) return `${b} B`
  if (b < 1024 * 1024) return `${(b / 1024).toFixed(1)} KB`
  return `${(b / 1024 / 1024).toFixed(2)} MB`
}

// path → 友好功能名 (徽章). install 根目录相对路径分类.
function fileRole(path: string): { label: string; core: boolean } {
  const p = path.toLowerCase()
  const base = p.split('/').pop() || p
  if (base === 'monitor_app.exe') return { label: '主程序', core: true }
  if (base.startsWith('updater')) return { label: '更新器', core: true }
  if (base === 'version.json') return { label: '清单', core: true }
  if (base.startsWith('logger')) return { label: '日志', core: false }
  if (base.startsWith('capture')) return { label: '捕获', core: false }
  if (base.startsWith('input')) return { label: '输入', core: false }
  if (p.startsWith('frontend/')) return { label: '界面', core: false }
  if (p.startsWith('config/')) return { label: '配置', core: false }
  return { label: '文件', core: false }
}

// 下载流量 (dl 优先, 缺省回退到解压大小).
const traffic = (f: { size?: number; dl?: number }) => f.dl ?? f.size ?? 0

export function UpdateModal({
  info,
  downloading,
  progress,
  onDownload,
  onForceUpdate,
  onClose,
}: {
  info: UpdateInfo
  downloading: boolean
  progress?: UpdateProgressMsg | null
  onDownload: () => void
  onForceUpdate?: () => void
  onClose: () => void
}) {
  const [expanded, setExpanded] = useState(false)

  const diff = info.diff || []
  const nFiles = diff.length
  const totalSize = diff.reduce((s, f) => s + (f.size || 0), 0)
  const totalDl = diff.reduce((s, f) => s + traffic(f), 0)
  const serverFull = info.mode === 'full'  // 服务端/min_version 强制全量

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center">
      {/* Backdrop */}
      <div className="absolute inset-0 bg-black/60" onClick={info.mandatory ? undefined : onClose} />
      {/* Card — 与 Select 弹窗同尺寸: 固定 520×min(560,85vh), body 内部滚动, 窗口永不变 */}
      <div className="relative bg-bg-primary rounded-xl ring-1 ring-inset ring-border w-[520px] min-h-[min(560px,85vh)] max-h-[min(560px,85vh)] flex flex-col shadow-2xl overflow-hidden">
        {/* Header */}
        <div className="flex items-center justify-between px-5 py-4 border-b border-border shrink-0">
          <div className="flex items-center gap-2.5">
            <Download className="w-5 h-5 text-accent" />
            <span className="font-semibold text-sm text-text-primary">发现新版本</span>
          </div>
          {!info.mandatory && (
            <button
              onClick={onClose}
              className="w-7 h-7 rounded-lg flex items-center justify-center hover:bg-bg-tertiary transition-colors"
            >
              <X className="w-4 h-4 text-text-secondary" />
            </button>
          )}
        </div>

        {/* Body (scrolls internally) */}
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

          {/* Full-package hint */}
          {serverFull && (
            <div className="text-xs text-text-secondary text-center">
              本次为完整更新（下载全部文件）
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
            <div className="bg-bg-secondary rounded-lg overflow-hidden">
              {/* Header row — whole bar toggles */}
              <button
                onClick={() => setExpanded((v) => !v)}
                className="w-full flex items-center gap-2 px-3 py-2.5 hover:bg-bg-tertiary/50 transition-colors text-left"
              >
                <span className="flex-1 min-w-0 text-xs text-text-secondary">
                  本次更新 · <span className="font-medium text-text-primary">{nFiles}</span> 个文件
                </span>
                {/* 解压总计 */}
                <span className="w-20 shrink-0 text-right leading-tight">
                  <span className="block text-[9px] text-text-muted">解压</span>
                  <span className="block text-[11px] font-mono font-medium text-text-primary tabular-nums">{fmtSize(totalSize)}</span>
                </span>
                {/* 流量总计 */}
                <span className="w-20 shrink-0 text-right leading-tight">
                  <span className="block text-[9px] text-text-muted">流量</span>
                  <span className="block text-[11px] font-mono font-medium text-accent tabular-nums">{fmtSize(totalDl)}</span>
                </span>
                {/* chevron gutter (w-5) — caption 占位使 chevron 下移到 number 行、
                    与解压/流量数字中线对齐; 文件行留等宽空槽保持列对齐 */}
                <span className="w-5 shrink-0 flex flex-col items-center leading-tight">
                  <span className="block text-[9px] leading-tight select-none">&nbsp;</span>
                  <ChevronDown
                    className={`w-4 h-4 text-text-muted transition-transform ${expanded ? 'rotate-180' : ''}`}
                  />
                </span>
              </button>

              {/* Expanded file list */}
              {expanded && (
                <div className="max-h-44 overflow-y-auto border-t border-border/60 divide-y divide-border/40">
                  {diff.map((f, i) => {
                    const role = fileRole(f.path)
                    return (
                      <div key={i} className="flex items-center gap-2 px-3 py-1.5">
                        {/* 友好名徽章 (第一深位, 固定宽 → 路径左对齐) */}
                        <span
                          className={`w-14 shrink-0 text-center text-[10px] py-0.5 rounded ring-1 ring-inset
                            ${role.core
                              ? 'bg-accent/10 text-accent ring-accent/30'
                              : 'bg-bg-tertiary text-text-muted ring-border'}`}
                        >
                          {role.label}
                        </span>
                        {/* 路径 (第二深位, install 根相对, 过长截断) */}
                        <span className="flex-1 min-w-0 truncate text-[11px] font-mono text-text-secondary">
                          {f.path}
                        </span>
                        {/* 解压大小 */}
                        <span className="w-20 shrink-0 text-right text-[11px] font-mono text-text-muted tabular-nums">
                          {fmtSize(f.size || 0)}
                        </span>
                        {/* 下载流量 (最右数据列) */}
                        <span className="w-20 shrink-0 text-right text-[11px] font-mono text-text-secondary tabular-nums">
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

        {/* Progress strip — 常驻固定高, idle 空占位, 下载中填充 → 加进度条窗口不跳 */}
        <div className="h-[52px] shrink-0 px-5 flex flex-col justify-center border-t border-border">
          {downloading && (
            progress ? (
              <div className="space-y-1.5">
                <div className="text-xs text-text-secondary">
                  {progress.phase === 'download' && (
                    <>正在下载 {progress.file || '...'} ({Math.min(progress.current_file, progress.total_files) || 1}/{progress.total_files})</>
                  )}
                  {progress.phase === 'done' && <>下载完成，正在重启安装…</>}
                  {progress.phase === 'error' && (
                    <span className="text-error">下载失败：{progress.error_file || progress.file}</span>
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
                正在准备下载…
              </div>
            )
          )}
        </div>

        {/* Footer */}
        <div className="flex items-center justify-end gap-2 px-5 py-3 border-t border-border shrink-0">
          {!info.mandatory && (
            <ActionBtn
              label="稍后"
              title="稍后再更新"
              icon={<X className="w-3.5 h-3.5" />}
              variant="outline"
              onClick={() => {
                addLog('[update] dismissed')
                onClose()
              }}
            />
          )}
          {/* 全量更新 — 强制重下全部文件 (逐文件覆盖, 非重装). serverFull 时增量按钮即全量, 隐藏此按钮避免重复 */}
          {!downloading && onForceUpdate && !serverFull && (
            <ActionBtn
              label="全量更新"
              title="强制下载全部文件（逐文件覆盖，用于本地文件损坏或增量疑漏）"
              icon={<Package className="w-3.5 h-3.5" />}
              variant="outline"
              onClick={() => {
                addLog('[update] force full update')
                onForceUpdate()
              }}
            />
          )}
          {/* 增量更新 (primary) — 只下 sha 变的文件. serverFull 时它下的就是全部 → 标为全量更新 */}
          <ActionBtn
            label={downloading ? '安装中…' : serverFull ? '全量更新' : '增量更新'}
            title={serverFull ? '下载全部文件并安装' : '只下载变化的文件并安装'}
            icon={serverFull ? <Package className="w-3.5 h-3.5" /> : <FileStack className="w-3.5 h-3.5" />}
            variant="primary"
            onClick={() => {
              addLog(`[update] downloading v${info.latest}`)
              onDownload()
            }}
          />
        </div>
      </div>
    </div>
  )
}
