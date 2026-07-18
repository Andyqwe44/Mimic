// ═══ DevTools View — standalone tab (visible when devMode is ON) ═══
// NOTE: "DevTools" ≠ "Dev build" — DevTools is the developer-tools panel
// accessible from both Dev and Prod builds via Settings → Dev mode toggle.
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Cpu, Play, Pencil, FolderOpen, Crosshair } from 'lucide-react'
import { Tooltip } from './Toolkit'
import { SettingsCard } from './SettingsView'
import { hostCall, addLog } from '../lib/bridge'

export function DevToolsView({
  appVersion,
  saveCaptureFrames, setSaveCaptureFrames,
  saveStreamFrames, setSaveStreamFrames,
  frameDumpDir, setFrameDumpDir,
  onRunSelfTest,
  selfTestRunning,
  onPreviewSkeleton,
  onDevInjectUpdate,
  onDevInjectDownload,
  onDevInjectSelfTest,
  onDevInjectAgent,
}: {
  appVersion: string
  saveCaptureFrames: boolean; setSaveCaptureFrames: (v: boolean) => void
  saveStreamFrames: boolean; setSaveStreamFrames: (v: boolean) => void
  frameDumpDir: string; setFrameDumpDir: (d: string) => void
  onRunSelfTest?: (perCell: number) => void
  selfTestRunning?: boolean
  onPreviewSkeleton?: () => void
  onDevInjectUpdate?: (info: any) => void
  onDevInjectDownload?: (phase: 'download' | 'done' | 'error') => void
  onDevInjectSelfTest?: (state: any) => void
  onDevInjectAgent?: (connected: boolean) => void
}) {
  const { t } = useTranslation()
  const [testTargetRunning, setTestTargetRunning] = useState(false)
  const [selfTestPerCell, setSelfTestPerCell] = useState(5)

  return (
    <div className="flex-1 overflow-y-auto p-6 space-y-3">
      {/* ── Test Target Launcher ── */}
      <SettingsCard
        icon={<Play className="w-4 h-4 text-accent-secondary" />}
        title={t('devtools.test_target')}
        defaultExpanded={true}
      >
        <div className="space-y-3">
          <div className="flex items-center justify-between">
            <div>
              <div className="text-sm text-text-primary">{t('devtools.test_target')}</div>
              <div className="text-xs text-text-muted">
                {t('devtools.test_target_desc')}
              </div>
            </div>
            <Tooltip text={testTargetRunning ? t('devtools.test_target_close_tip') : t('devtools.test_target_launch_tip')}>
            <button
              onClick={() => {
                hostCall('launch_test_target')
                  .then((res: any) => {
                    if (res?.ok) {
                      const action = res?.action || 'launched'
                      setTestTargetRunning(action === 'launched')
                      addLog(`[Dev] test target ${action}`)
                    } else {
                      addLog(`[Dev] test target failed: ${res?.error || '?'}`)
                    }
                  })
                  .catch((err: any) => addLog(`[Dev] test target error: ${err?.message || err}`))
              }}
              className={`inline-flex items-center gap-1.5 h-7 px-3 rounded-md text-xs font-medium border transition-colors ${
                testTargetRunning
                  ? 'border-success-ring bg-success-soft text-success hover:bg-success-soft-mid'
                  : 'border-accent-secondary-ring bg-accent-secondary-soft text-accent-secondary hover:bg-accent-secondary-soft-mid'
              }`}
            >
              <Play className={`w-3 h-3 ${testTargetRunning ? 'fill-current' : ''}`} />
              {testTargetRunning ? t('devtools.test_target_close') : t('devtools.test_target_launch')}
            </button>
            </Tooltip>
          </div>
        </div>
      </SettingsCard>

      {/* ── Self-Test (mapping calibration) ── */}
      <SettingsCard
        icon={<Crosshair className="w-4 h-4 text-accent-secondary" />}
        title={t('devtools.selftest')}
        defaultExpanded={true}
      >
        <div className="space-y-3">
          <div>
            <div className="text-xs text-text-muted mb-2">
              {t('devtools.selftest_desc')}
            </div>
            <div className="flex items-center gap-2">
              <Tooltip text={t('devtools.selftest_density_tip')}>
                <select
                  value={selfTestPerCell}
                  onChange={(e) => setSelfTestPerCell(Number(e.target.value))}
                  disabled={selfTestRunning}
                  className="h-7 rounded-lg border border-border bg-bg-primary px-2 text-xs outline-none focus:border-accent disabled:opacity-50"
                >
                  {[3, 5, 8].map((n) => (
                    <option key={n} value={n}>{n}×{n}{t('devtools.selftest_density_unit')}</option>
                  ))}
                </select>
              </Tooltip>
              <Tooltip text={selfTestRunning ? t('devtools.selftest_running_tip') : t('devtools.selftest_run_tip')}>
                <button
                  onClick={() => onRunSelfTest?.(selfTestPerCell)}
                  disabled={selfTestRunning}
                  className={`inline-flex items-center gap-1.5 h-7 px-3 rounded-md text-xs font-medium border transition-colors ${
                    selfTestRunning
                      ? 'border-border bg-bg-tertiary text-text-muted cursor-not-allowed'
                      : 'border-accent-ring bg-accent-soft text-accent hover:bg-accent-soft-mid'
                  }`}
                >
                  <Crosshair className="w-3 h-3" />
                  {selfTestRunning ? t('devtools.selftest_running') : t('devtools.selftest_run')}
                </button>
              </Tooltip>
            </div>
          </div>
        </div>
      </SettingsCard>

      {/* ── Frame Dump ── */}
      <SettingsCard
        icon={<Pencil className="w-4 h-4 text-accent-secondary" />}
        title={t('devtools.frame_dump')}
        defaultExpanded={true}
      >
        <div className="space-y-3">
          <div className="flex items-center justify-between">
            <div>
              <div className="text-sm text-text-primary">{t('devtools.frame_dump_capture')}</div>
              <div className="text-xs text-text-muted">
                {t('devtools.frame_dump_capture_desc')}
              </div>
            </div>
            <Tooltip text={t('devtools.frame_dump_capture_tip')}>
            <button
              onClick={() => {
                const v = !saveCaptureFrames
                setSaveCaptureFrames(v)
                hostCall('set_frame_dump', {
                  capture: v, stream: saveStreamFrames, dir: frameDumpDir,
                }).catch(() => {})
              }}
              className={`relative w-10 h-5 rounded-full transition-colors ${saveCaptureFrames ? 'bg-success' : 'bg-bg-tertiary'}`}
            >
              <span className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${saveCaptureFrames ? 'translate-x-5' : ''}`} />
            </button>
            </Tooltip>
          </div>
          <div className="flex items-center justify-between">
            <div>
              <div className="text-sm text-text-primary">{t('devtools.frame_dump_stream')}</div>
              <div className="text-xs text-text-muted">
                {t('devtools.frame_dump_stream_desc')}
              </div>
            </div>
            <Tooltip text={t('devtools.frame_dump_stream_tip')}>
            <button
              onClick={() => {
                const v = !saveStreamFrames
                setSaveStreamFrames(v)
                hostCall('set_frame_dump', {
                  capture: saveCaptureFrames, stream: v, dir: frameDumpDir,
                }).catch(() => {})
              }}
              className={`relative w-10 h-5 rounded-full transition-colors ${saveStreamFrames ? 'bg-success' : 'bg-bg-tertiary'}`}
            >
              <span className={`absolute top-0.5 left-0.5 w-4 h-4 rounded-full bg-white transition-transform ${saveStreamFrames ? 'translate-x-5' : ''}`} />
            </button>
            </Tooltip>
          </div>
          <div className="flex items-center gap-2">
            <label className="text-sm text-text-secondary w-24 shrink-0">{t('devtools.frame_dump_dir')}</label>
            <Tooltip text={t('devtools.frame_dump_dir_tip')} className="flex-1 min-w-0">
              <input
                value={frameDumpDir || t('devtools.frame_dump_not_set')}
                readOnly
                className="w-full h-7 rounded-lg border border-border bg-bg-primary px-3 text-sm text-text-muted outline-none cursor-default font-mono text-xs truncate"
              />
            </Tooltip>
            <Tooltip text={t('devtools.frame_dump_pick_dir_tip')}>
              <button
                onClick={async () => {
                  try {
                    const res = await hostCall('pick_dir')
                    if (res?.dir) {
                      setFrameDumpDir(res.dir)
                      if (!saveCaptureFrames) setSaveCaptureFrames(true)
                      if (!saveStreamFrames) setSaveStreamFrames(true)
                      hostCall('set_frame_dump', {
                        capture: true, stream: true, dir: res.dir,
                      }).catch(() => {})
                      addLog(`[Dev] dump dir = ${res.dir}`)
                    }
                  } catch (_) {}
                }}
                className="shrink-0 p-1.5 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors"
              >
                <Pencil className="w-4 h-4" />
              </button>
            </Tooltip>
            <Tooltip text={t('devtools.frame_dump_open_dir_tip')}>
              <button
                onClick={() => {
                  if (frameDumpDir)
                    hostCall('open_dir', { dir: frameDumpDir }).catch(() => {})
                }}
                className="shrink-0 p-1.5 rounded-md text-text-secondary hover:text-text-primary hover:bg-bg-tertiary transition-colors"
              >
                <FolderOpen className="w-4 h-4" />
              </button>
            </Tooltip>
          </div>
        </div>
      </SettingsCard>

      {/* ── Misc ── */}
      <SettingsCard
        icon={<Cpu className="w-4 h-4 text-accent-secondary" />}
        title={t('devtools.misc')}
        defaultExpanded={true}
      >
        <div className="space-y-3">
          <div className="flex items-center justify-between">
            <div>
              <div className="text-sm text-text-primary">{t('devtools.misc_skeleton')}</div>
              <div className="text-xs text-text-muted">{t('devtools.misc_skeleton_desc')}</div>
            </div>
            <Tooltip text={t('devtools.misc_skeleton_tip')}>
              <button
                onClick={() => onPreviewSkeleton?.()}
                className="px-3 h-7 rounded-lg text-xs bg-bg-tertiary hover:opacity-80 text-text-primary transition-opacity"
              >
                {t('devtools.misc_skeleton_btn')}
              </button>
            </Tooltip>
          </div>
        </div>
      </SettingsCard>

      {/* ── UI Demos ── */}
      <SettingsCard
        icon={<Cpu className="w-4 h-4 text-accent-secondary" />}
        title={t('devtools.ui_demos')}
        defaultExpanded={true}
      >
        <div className="space-y-3">
          <div className="text-xs text-text-muted">
            {t('devtools.ui_demos_desc')}
          </div>

          {/* ─ Update Modal demos ─ */}
          <div className="text-xs font-medium text-accent-secondary mb-1.5">Update Modal</div>
          <div className="flex flex-wrap gap-1.5 mb-3">
            {([
              [t('devtools.demo_checking'), () => onDevInjectUpdate?.({
                status: 'checking', current: appVersion.replace(/^v/, ''),
                latest: '', name: '', body: '', url: '', _dev: true,
              })],
              [t('devtools.demo_latest'), () => onDevInjectUpdate?.({
                status: 'latest', current: appVersion.replace(/^v/, ''),
                latest: appVersion.replace(/^v/, ''), name: '', body: '', url: '', _dev: true,
              })],
              [t('devtools.demo_update'), () => {
                const cur = appVersion.replace(/^v/, '')
                onDevInjectUpdate?.({
                  status: 'update', current: cur, latest: '9.9.99',
                  name: 'v9.9.99 — Demo Release (模拟数据)',
                  body: '🚀 新功能\n- 这是一条模拟的更新日志\n- 用于测试 UpdateModal 在有更新时的展示效果\n\n🐛 修复\n- 修复了一个不存在的问题\n- 又修复了另一个不存在的问题',
                  url: '',
                  diff: [
                    { path: 'bin/mimic_client.exe', size: 524288, dl: 491520 },
                    { path: 'bin/updater.exe', size: 131072, dl: 122880 },
                    { path: 'bin/logger.dll', size: 98304, dl: 90112 },
                    { path: 'frontend/assets/index-Cys4Z6Yf.js', size: 204800, dl: 196608 },
                    { path: 'frontend/assets/index-ByT4uD_r.css', size: 65536, dl: 61440 },
                    { path: 'frontend/index.html', size: 2048, dl: 2048 },
                  ],
                  message: '推荐更新，包含重要功能改进。',
                  mandatory: false, mode: 'incremental', _dev: true,
                })
              }],
              [t('devtools.demo_error'), () => onDevInjectUpdate?.({
                status: 'error', current: appVersion.replace(/^v/, ''),
                latest: '', name: '', body: '', url: '',
                error: '无法连接更新服务器 (模拟错误)\n请检查网络连接后重试。',
                _dev: true,
              })],
            ] as [string, () => void][]).map(([label, fn]) => (
              <button key={label} onClick={fn}
                className="px-2.5 h-7 rounded-md text-[11px] font-medium bg-accent-secondary-soft text-accent-secondary hover:bg-accent-secondary-soft-mid border border-accent-secondary-ring transition-colors">
                {label}
              </button>
            ))}
          </div>
          <div className="flex flex-wrap gap-1.5 mb-3">
            {([
              [t('devtools.demo_mandatory'), () => {
                const cur = appVersion.replace(/^v/, '')
                onDevInjectUpdate?.({
                  status: 'update', current: cur, latest: '9.9.99',
                  name: 'v9.9.99 — 重要安全更新', body: '此版本包含关键安全修复，必须立即更新。', url: '',
                  diff: [{ path: 'bin/mimic_client.exe', size: 524288, dl: 491520 }, { path: 'frontend/index.html', size: 2048, dl: 2048 }],
                  message: '⚠️ 模拟强制更新 — 真实版会隐藏关闭按钮。Dev 模式保留所有关闭入口。',
                  mandatory: false, mode: 'incremental', _dev: true,
                })
              }],
              [t('devtools.demo_full'), () => {
                const cur = appVersion.replace(/^v/, '')
                onDevInjectUpdate?.({
                  status: 'update', current: cur, latest: '9.9.99',
                  name: 'v9.9.99 — Full Package', body: 'min_version 高于当前版本，强制全量下载。', url: '',
                  diff: [
                    { path: 'bin/mimic_client.exe', size: 524288, dl: 491520 },
                    { path: 'bin/updater.exe', size: 131072, dl: 122880 },
                    { path: 'bin/logger.dll', size: 98304, dl: 90112 },
                    { path: 'bin/capture_wgc.dll', size: 147456, dl: 131072 },
                    { path: 'bin/capture_gdi.dll', size: 81920, dl: 73728 },
                    { path: 'bin/capture_pw.dll', size: 81920, dl: 73728 },
                    { path: 'bin/capture_screen.dll', size: 81920, dl: 73728 },
                    { path: 'bin/capture_desktop.dll', size: 81920, dl: 73728 },
                    { path: 'bin/capture_common.dll', size: 65536, dl: 57344 },
                    { path: 'bin/capture_dxgi.dll', size: 131072, dl: 114688 },
                    { path: 'bin/input_sendinput.dll', size: 65536, dl: 57344 },
                    { path: 'bin/input_winapi.dll', size: 65536, dl: 57344 },
                    { path: 'bin/input_postmessage.dll', size: 65536, dl: 57344 },
                    { path: 'bin/input_driver.dll', size: 49152, dl: 40960 },
                    { path: 'bin/input_common.dll', size: 65536, dl: 57344 },
                    { path: 'frontend/assets/index-Cys4Z6Yf.js', size: 204800, dl: 196608 },
                    { path: 'frontend/assets/index-ByT4uD_r.css', size: 65536, dl: 61440 },
                    { path: 'frontend/index.html', size: 2048, dl: 2048 },
                    { path: 'config/settings.default.json', size: 4096, dl: 2048 },
                  ],
                  message: '版本跨度较大，需完整下载全部文件。', mandatory: false, mode: 'full', _dev: true,
                })
              }],
              [t('devtools.demo_download'), () => onDevInjectDownload?.('download')],
              [t('devtools.demo_download_fail'), () => onDevInjectDownload?.('error')],
              [t('devtools.demo_resume'), () => {
                const cur = appVersion.replace(/^v/, '')
                const fakeDiff = [
                  { path: 'bin/mimic_client.exe', size: 524288, dl: 491520 },
                  { path: 'bin/logger.dll', size: 98304, dl: 98304 },
                  { path: 'bin/capture_wgc.dll', size: 65536, dl: 65536 },
                  { path: 'frontend/index.html', size: 2048, dl: 2048 },
                  { path: 'frontend/assets/index-xxx.js', size: 327680, dl: 294912 },
                  { path: 'frontend/assets/index-xxx.css', size: 16384, dl: 8192 },
                  { path: 'config/settings.default.json', size: 4096, dl: 2048 },
                ]
                onDevInjectUpdate?.({
                  status: 'update', current: cur, latest: '99.99.99',
                  name: '', body: '', url: '',
                  diff: fakeDiff,
                  mode: 'incremental', mandatory: false,
                  staging_state: {
                    has_partial: true, done_files: 3, total_files: 7,
                    done_bytes: 688128, total_bytes: 1048320,
                    done_paths: ['bin/mimic_client.exe', 'bin/logger.dll', 'bin/capture_wgc.dll'],
                  },
                  _dev: true,
                })
              }],
            ] as [string, () => void][]).map(([label, fn]) => (
              <button key={label} onClick={fn}
                className="px-2.5 h-7 rounded-md text-[11px] font-medium bg-accent-secondary-soft text-accent-secondary hover:bg-accent-secondary-soft-mid border border-accent-secondary-ring transition-colors">
                {label}
              </button>
            ))}
          </div>

          {/* ─ Self-Test demos ─ */}
          <div className="text-xs font-medium text-accent-secondary mb-1.5">Self-Test Modal</div>
          <div className="flex flex-wrap gap-1.5 mb-3">
            {([
              [t('devtools.demo_selftest_running'), () => onDevInjectSelfTest?.({ phase: 'running', done: 312, total: 625 })],
              [t('devtools.demo_selftest_done'), () => {
                const grid = 5
                const cells: number[][] = []
                const cellCounts: number[][] = []
                for (let gy = 0; gy < grid; gy++) {
                  cells[gy] = []
                  cellCounts[gy] = []
                  for (let gx = 0; gx < grid; gx++) {
                    cellCounts[gy][gx] = 25
                    const base = 0.95 - (gx + gy) * 0.08
                    cells[gy][gx] = Math.max(0.3, Math.min(1, base + (Math.random() - 0.5) * 0.1))
                  }
                }
                onDevInjectSelfTest?.({
                  phase: 'done',
                  summary: {
                    geo: { client_w: 400, client_h: 400, grid: 5, cell: 64, pad: 40, hit_margin: 16 },
                    total: 625, received: 618, cellMatch: 607, hitMatch: 582,
                    meanDx: 0.8, meanDy: -1.2, meanAbs: 2.1, maxAbs: 5.4,
                    cells, cellCounts, points: [], aborted: false,
                    scenarios: [
                      { id: 'right_click', label: 'Right click', ok: true, detail: 'btn=2 grid[2,2]' },
                      { id: 'wheel', label: 'Scroll wheel', ok: true, detail: 'delta=120' },
                      { id: 'drag', label: 'Drag', ok: true, detail: 'box=(120,50)' },
                      { id: 'button', label: 'Button A', ok: true, detail: 'id=btn-a' },
                      { id: 'text', label: 'Text input', ok: false, detail: 'no feedback (demo)' },
                    ],
                  },
                })
              }],
              [t('devtools.demo_selftest_error'), () => onDevInjectSelfTest?.({ phase: 'error', error: 'test_target 连接超时 (模拟)' })],
            ] as [string, () => void][]).map(([label, fn]) => (
              <button key={label} onClick={fn}
                className="px-2.5 h-7 rounded-md text-[11px] font-medium bg-accent-secondary-soft text-accent-secondary hover:bg-accent-secondary-soft-mid border border-accent-secondary-ring transition-colors">
                {label}
              </button>
            ))}
          </div>

          {/* ─ Agent demos ─ */}
          <div className="text-xs font-medium text-accent-secondary mb-1.5">{t('devtools.ui_demos_agent')}</div>
          <div className="flex flex-wrap gap-1.5">
            {([
              [t('devtools.demo_agent_connected'), () => onDevInjectAgent?.(true)],
              [t('devtools.demo_agent_disconnected'), () => onDevInjectAgent?.(false)],
            ] as [string, () => void][]).map(([label, fn]) => (
              <button key={label} onClick={fn}
                className="px-2.5 h-7 rounded-md text-[11px] font-medium bg-accent-secondary-soft text-accent-secondary hover:bg-accent-secondary-soft-mid border border-accent-secondary-ring transition-colors">
                {label}
              </button>
            ))}
          </div>
        </div>
      </SettingsCard>

      <div className="h-4" />
    </div>
  )
}
