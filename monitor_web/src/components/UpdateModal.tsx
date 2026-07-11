// ═══ UpdateModal — new version available, changelog + download ═══
import { X, Download, ArrowRight } from 'lucide-react'
import { ActionBtn } from './Toolkit'
import { addLog, type UpdateProgressMsg } from '../lib/bridge'

export interface UpdateInfo {
  current: string
  latest: string
  name: string
  body: string
  url: string
}

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
  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center">
      {/* Backdrop */}
      <div className="absolute inset-0 bg-black/60" onClick={onClose} />
      {/* Card */}
      <div className="relative bg-bg-primary rounded-xl ring-1 ring-inset ring-border w-[420px] max-h-[80vh] flex flex-col shadow-2xl">
        {/* Header */}
        <div className="flex items-center justify-between px-5 py-4 border-b border-border shrink-0">
          <div className="flex items-center gap-2.5">
            <Download className="w-5 h-5 text-accent" />
            <span className="font-semibold text-sm text-text-primary">Update Available</span>
          </div>
          <button
            onClick={onClose}
            className="w-7 h-7 rounded-lg flex items-center justify-center hover:bg-bg-tertiary transition-colors"
          >
            <X className="w-4 h-4 text-text-secondary" />
          </button>
        </div>

        {/* Body */}
        <div className="px-5 py-4 space-y-3 overflow-y-auto">
          {/* Version comparison */}
          <div className="flex items-center justify-center gap-3 py-2">
            <span className="px-3 py-1.5 rounded-lg bg-bg-tertiary text-sm font-mono text-text-muted">
              v{info.current}
            </span>
            <ArrowRight className="w-4 h-4 text-accent" />
            <span className="px-3 py-1.5 rounded-lg bg-accent/10 ring-1 ring-inset ring-accent/30 text-sm font-mono font-semibold text-accent">
              v{info.latest}
            </span>
          </div>

          {/* Release name */}
          {info.name && (
            <div className="text-sm font-medium text-text-primary text-center">
              {info.name}
            </div>
          )}

          {/* Changelog */}
          {info.body && (
            <div className="bg-bg-secondary rounded-lg p-3 max-h-48 overflow-y-auto">
              <div className="text-xs text-text-secondary whitespace-pre-wrap font-mono leading-relaxed">
                {info.body}
              </div>
            </div>
          )}

          {downloading && (
            progress ? (
              <div className="space-y-2">
                {/* Activity text describing the current step */}
                <div className="text-xs text-text-secondary">
                  {progress.phase === 'download' && (
                    <>正在下载 {progress.file || '...'} ({Math.min(progress.current_file, progress.total_files) || 1}/{progress.total_files})</>
                  )}
                  {progress.phase === 'done' && <>下载完成，正在重启安装…</>}
                  {progress.phase === 'error' && (
                    <span className="text-error">下载失败：{progress.error_file || progress.file}</span>
                  )}
                </div>
                {/* Progress bar (byte-level; falls back to file count / indeterminate) */}
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
                Downloading update...
              </div>
            )
          )}
        </div>

        {/* Footer */}
        <div className="flex items-center justify-end gap-2 px-5 py-3 border-t border-border shrink-0">
          <ActionBtn
            label="Later"
            title="Later"
            icon={<X className="w-3.5 h-3.5" />}
            variant="outline"
            onClick={() => {
              addLog('[update] dismissed')
              onClose()
            }}
          />
          {!downloading && onForceUpdate && (
            <ActionBtn
              label="完整更新"
              title="强制下载完整安装包（忽略增量，用于更新逻辑本身的改动）"
              icon={<Download className="w-3.5 h-3.5" />}
              variant="outline"
              onClick={() => {
                addLog('[update] force full update')
                onForceUpdate()
              }}
            />
          )}
          <ActionBtn
            label={downloading ? 'Installing...' : 'Download & Install'}
            title="Download & Install"
            icon={<Download className="w-3.5 h-3.5" />}
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
