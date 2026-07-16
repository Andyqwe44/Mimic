// ═══ App — Game Agent Monitor root ═══
// Orchestrates: tab routing, right-panel layout, capture state machine,
// theme, pin lock, window polling, and resize handling.
import { useState, useEffect, useRef, useCallback } from 'react'
import { useTranslation } from 'react-i18next'
import { TopBar } from './components/TopBar'
import { BottomBar } from './components/BottomBar'
import { Tooltip } from './components/Toolkit'
import { ConnectionPanel } from './components/ConnectionPanel'
import { ScreenshotPanel } from './components/ScreenshotPanel'
import { LogPanel } from './components/LogPanel'
import { SettingsView } from './components/SettingsView'
import { DevToolsView } from './components/DevToolsView'
import { MonitorView, type MonitorApi } from './components/MonitorView'
import { SelfTestModal } from './components/SelfTestModal'
import { UpdateModal, type UpdateInfo } from './components/UpdateModal'
import { LoadingScreen } from './components/LoadingScreen'
import { hostCall, logMgr, addLog, applyTheme, onUpdateProgress, type UpdateProgressMsg } from './lib/bridge'
import { hasBootSettings, readBootSettings } from './lib/bootSettings'
import { runSelfTest, sleep, type SelfTestState } from './lib/selftest'
import { cantCaptureMinimized } from './lib/constants'
import { DESKTOP_TITLE, displayTargetTitle } from './lib/windowTitle'
import { setSavedLocale } from './lib/i18n'
import type { WindowInfo, Rect } from './lib/types'

// ── Layout constants ──
const MIN_LEFT_WIDTH = 360
const DEFAULT_RIGHT_WIDTH = 324

// DELIBERATE test delay so the startup skeleton screen stays visible long enough
// to observe during development. Real startup needs NO artificial wait — the
// skeleton would otherwise flash by. Set to 0 (or delete the timer) for production.
const SPLASH_TEST_MS = 0

export default function App() {
  const { t, i18n } = useTranslation()

  // ═══ UI state ═══
  const [tab, setTab] = useState<'Monitor' | 'Log' | 'Settings' | 'DevTools'>('Settings')
  const [running, setRunning] = useState(false)
  // Initialised from the compile-time version (version.h via Vite) so the splash
  // shows it instantly; get_version overwrites it at runtime (they match).
  const [appVersion, setAppVersion] = useState(`v${__APP_VERSION__}`)
  const [appReady, setAppReady] = useState(false)   // false = show startup splash overlay
  const [previewSkeleton, setPreviewSkeleton] = useState(false)  // Dev: manual skeleton preview
  const [isAdmin, setIsAdmin] = useState(false)  // current process elevation (admin?)
  // ── Update SSOT (real check_update / download only) ──
  const [updateInfo, setUpdateInfo] = useState<UpdateInfo | null>(null)
  const [updateDownloading, setUpdateDownloading] = useState(false)
  const [updateProgress, setUpdateProgress] = useState<UpdateProgressMsg | null>(null)
  // ── Dev UI overlays — never written into SSOT (企业级：display = overlay ?? real) ──
  const [demoAgentOverride, setDemoAgentOverride] = useState<boolean | null>(null)
  const [demoUpdateInfo, setDemoUpdateInfo] = useState<UpdateInfo | null>(null)
  const [demoUpdateDownloading, setDemoUpdateDownloading] = useState(false)
  const [demoUpdateProgress, setDemoUpdateProgress] = useState<UpdateProgressMsg | null>(null)
  const [demoSelfTest, setDemoSelfTest] = useState<SelfTestState | null>(null)
  const [rightWidth, setRightWidth] = useState(DEFAULT_RIGHT_WIDTH)
  const [rightCollapsed, setRightCollapsed] = useState(false)

  // ── Right panel expanded/collapsed state + refs (used in layout callbacks) ──
  const [connectionExpanded, setConnectionExpanded] = useState(true)
  const [screenshotExpanded, setScreenshotExpanded] = useState(false)
  const [logExpanded, setLogExpanded] = useState(true)
  const connectionExpandedRef = useRef(connectionExpanded)
  connectionExpandedRef.current = connectionExpanded
  const screenshotExpandedRef = useRef(screenshotExpanded)
  screenshotExpandedRef.current = screenshotExpanded
  const logExpandedRef = useRef(logExpanded)
  logExpandedRef.current = logExpanded

  // ── Pin lock — prevents auto-layout from changing pinned panels ──
  // null = not pinned; true/false = locked to that expanded state
  const connPinLocked = useRef<boolean | null>(null)
  const ssPinLocked = useRef<boolean | null>(null)
  const logPinLocked = useRef<boolean | null>(null)
  const [connectionPinned, setConnectionPinned] = useState(false)
  const [screenshotPinned, setScreenshotPinned] = useState(false)
  const [logPinned, setLogPinned] = useState(false)
  const ssHasContentRef = useRef(false)   // tracks whether Screenshot canvas has rendered content

  // ── Safe setters — check pin lock before modifying expanded state ──
  // Return false when pinned (caller can detect rejection); true when state was set
  const setConnExpanded = useCallback((v: boolean): boolean => {
    if (connPinLocked.current !== null) return false
    setConnectionExpanded(v)
    return true
  }, [])
  const setSsExpanded = useCallback((v: boolean): boolean => {
    if (ssPinLocked.current !== null) return false
    setScreenshotExpanded(v)
    return true
  }, [])
  const setLogPanelExpanded = useCallback((v: boolean): boolean => {
    if (logPinLocked.current !== null) return false
    setLogExpanded(v)
    return true
  }, [])

  // ── Pin toggles — record current expanded state as lock, or release lock ──
  const toggleConnPin = useCallback(() => {
    if (connPinLocked.current === null) {
      connPinLocked.current = connectionExpandedRef.current
      setConnectionPinned(true)
    } else {
      connPinLocked.current = null
      setConnectionPinned(false)
    }
  }, [])
  const toggleSsPin = useCallback(() => {
    if (ssPinLocked.current === null) {
      ssPinLocked.current = screenshotExpandedRef.current
      setScreenshotPinned(true)
    } else {
      ssPinLocked.current = null
      setScreenshotPinned(false)
    }
  }, [])
  const toggleLogPin = useCallback(() => {
    if (logPinLocked.current === null) {
      logPinLocked.current = logExpandedRef.current
      setLogPinned(true)
    } else {
      logPinLocked.current = null
      setLogPinned(false)
    }
  }, [])
  const rightPanelRef = useRef<HTMLDivElement>(null)

  // ═══ Theme ═══
  // Boot snapshot from C++ (pre-paint) — avoids default-blue flash on startup.
  const boot = readBootSettings()
  const [theme, setTheme] = useState<'light' | 'dark' | 'system'>(
    () => (boot.theme === 'light' || boot.theme === 'dark' || boot.theme === 'system' ? boot.theme : 'light'),
  )
  const [systemDark, setSystemDark] = useState(false)
  const resolvedDark = theme === 'system' ? systemDark : theme === 'dark'

  // ═══ Locale ═══
  const [locale, setLocaleState] = useState(
    () => (typeof boot.locale === 'string' && boot.locale ? boot.locale : (i18n.language || 'zh-CN')),
  )
  const setLocale = useCallback((l: string) => {
    setLocaleState(l)
    i18n.changeLanguage(l)
    setSavedLocale(l)
    addLog(`[Lang] ${l}`)
  }, [i18n])

  // ── Listen for OS-level dark mode changes ──
  useEffect(() => {
    const mq = window.matchMedia('(prefers-color-scheme: dark)')
    setSystemDark(mq.matches)
    const onChange = (e: MediaQueryListEvent) => setSystemDark(e.matches)
    mq.addEventListener('change', onChange)
    return () => mq.removeEventListener('change', onChange)
  }, [])

  // ── Apply theme CSS class whenever resolvedDark changes ──
  useEffect(() => {
    applyTheme(resolvedDark)
  }, [resolvedDark])

  // ═══ Right panel auto-layout ═══
  // Measures collapsed/expanded heights, then auto-collapses/expands panels
  // as the window resizes or the divider is dragged.
  // H = measured heights: C/S/L = expanded, Cp/Sp/Lp = collapsed (header only)
  const H = useRef({ C: 180, S: 300, L: 250, Cp: 44, Sp: 44, Lp: 44 })
  const GAP = 60                         // gap between panels (p-3 = 12px × 2 + gap-3 = 12px ≈ 36px total; extra for safety)
  const prevClientH = useRef(0)          // previous container height (detect grow vs shrink)
  const guard = useRef({ C: 0, S: 0, L: 0 })  // debounce timestamps per panel

  // ── Measure actual rendered heights of each panel ──
  const measureLayout = useCallback(() => {
    const el = rightPanelRef.current
    if (!el) return
    const kids = el.querySelectorAll(':scope > div')
    if (kids.length < 3) return
    const gh = (i: number) => (kids[i] as HTMLElement).offsetHeight
    if (connectionExpandedRef.current) H.current.C = gh(0)
    else H.current.Cp = gh(0)
    if (screenshotExpandedRef.current) H.current.S = gh(1)
    else H.current.Sp = gh(1)
    if (logExpandedRef.current) H.current.L = gh(2)
    else H.current.Lp = gh(2)
    if (!connectionExpandedRef.current) {
      const inner = kids[0].querySelector('[data-layout-measure]') as HTMLElement | null
      if (inner) H.current.C = Math.max(H.current.C, gh(0) + inner.scrollHeight)
    }
    if (!screenshotExpandedRef.current) {
      const inner = kids[1].querySelector('[data-layout-measure]') as HTMLElement | null
      if (inner) H.current.S = Math.max(H.current.S, gh(1) + inner.scrollHeight)
    }
    if (!logExpandedRef.current) {
      const inner = kids[2].querySelector('[data-layout-measure]') as HTMLElement | null
      if (inner) H.current.L = Math.max(H.current.L, gh(2) + inner.scrollHeight)
    }
  }, [])

  useEffect(() => {
    const t = setTimeout(measureLayout, 200)
    return () => clearTimeout(t)
  }, [connectionExpanded, screenshotExpanded, logExpanded, measureLayout])

  useEffect(() => {
    measureLayout()
  }, [])

  // Version + LogManager init
  useEffect(() => {
    hostCall('get_version')
      .then((v: string) => {
        if (v) setAppVersion(v.startsWith('v') ? v : `v${v}`)
      })
      .catch(() => {})
  }, [])

  // Reveal the host window once React has painted its first frame, then drop the
  // startup splash after a brief beat. The window starts HIDDEN on the C++ side to
  // hide the ~2-4s WebView2 startup gap, so it appears already showing the splash
  // spinner instead of a white blank. Double rAF ensures layout + paint have
  // flushed (splash visible) before we ask the host to show the window.
  useEffect(() => {
    let raf1 = 0
    let raf2 = 0
    raf1 = requestAnimationFrame(() => {
      raf2 = requestAnimationFrame(() => {
        hostCall('show_window').catch(() => {})
      })
    })
    // Dev preview: open with ?splash to freeze the skeleton (it otherwise clears
    // after SPLASH_TEST_MS). View at http://localhost:1425/?splash in a browser.
    const holdSplash = new URLSearchParams(window.location.search).has('splash')
    // Keep the skeleton up a deliberate beat (test only), then reveal the main UI.
    const t = holdSplash ? 0 : window.setTimeout(() => setAppReady(true), SPLASH_TEST_MS)
    return () => {
      cancelAnimationFrame(raf1)
      cancelAnimationFrame(raf2)
      if (t) clearTimeout(t)
    }
  }, [])

  // 预览骨架屏（开发人员）：显示 3 秒后自动消失，避免全屏遮罩关不掉。
  const previewSkeletonScreen = useCallback(() => {
    setPreviewSkeleton(true)
    window.setTimeout(() => setPreviewSkeleton(false), 3000)
    addLog('[Dev] 预览骨架屏 (3s)')
  }, [])

  // 运行权限：加载当前进程是否管理员；切换 = 重启到目标权限（升→UAC，降→token 降级）。
  useEffect(() => {
    hostCall('get_elevation').then((r: any) => setIsAdmin(!!r?.admin)).catch(() => {})
  }, [])
  const switchPermission = useCallback((toAdmin: boolean) => {
    addLog(`[Perm] 切换到${toAdmin ? '管理员' : '普通'}权限，重启程序…`)
    hostCall('switch_permission', { admin: toAdmin }).catch(() => {})
  }, [])

  // ── Dev UI demos: write overlays only — never SSOT (铁律 5 / 企业级) ──
  const devInjectUpdate = useCallback((info: UpdateInfo) => {
    setDemoUpdateInfo({ ...info, _dev: true })
    setDemoUpdateDownloading(false)
    setDemoUpdateProgress(null)
    addLog(`[Dev] overlay update: status=${info.status}`)
  }, [])
  const devInjectSelfTest = useCallback((st: SelfTestState) => {
    setDemoSelfTest({ ...st, _dev: true } as SelfTestState)
    addLog(`[Dev] overlay selftest: phase=${st.phase}`)
  }, [])
  const devInjectAgent = useCallback((connected: boolean) => {
    setDemoAgentOverride(connected)
    addLog(`[Dev] overlay agent: ${connected ? 'connected' : 'disconnected'}`)
  }, [])

  // Fake download progress — overlay only, no hostCall
  const devInjectDownload = useCallback((phase: 'download' | 'done' | 'error') => {
    const cur = appVersion.replace(/^v/, '')
    const fakeDiff = [
      { path: 'bin/monitor_app.exe', size: 524288, dl: 491520 },
      { path: 'bin/updater.exe', size: 131072, dl: 122880 },
      { path: 'bin/updater.new', size: 131072, dl: 122880 },
      { path: 'frontend/assets/index-Cys4Z6Yf.js', size: 204800, dl: 196608 },
      { path: 'frontend/assets/index-ByT4uD_r.css', size: 65536, dl: 61440 },
      { path: 'frontend/index.html', size: 2048, dl: 2048 },
    ]
    setDemoUpdateInfo({
      status: 'update', current: cur, latest: '9.9.99',
      name: 'v9.9.99 — Demo Download (模拟)', body: '', url: '',
      diff: fakeDiff, message: '', mandatory: false, mode: 'incremental', _dev: true,
    })
    setDemoUpdateDownloading(true)
    if (phase === 'download') {
      setDemoUpdateProgress({ phase: 'download', current_file: 3, total_files: 6, skipped_files: 0, file: 'frontend/assets/index-Cys4Z6Yf.js', done_bytes: 460800, total_bytes: 1048576, skipped_bytes: 0 })
    } else if (phase === 'done') {
      setDemoUpdateProgress({ phase: 'done', current_file: 6, total_files: 6, skipped_files: 0, file: '', done_bytes: 1048576, total_bytes: 1048576, skipped_bytes: 0 })
    } else {
      setDemoUpdateProgress({ phase: 'error', current_file: 3, total_files: 6, skipped_files: 0, file: 'frontend/index.html', done_bytes: 460800, total_bytes: 1048576, skipped_bytes: 0, error_file: 'frontend/index.html' })
    }
    addLog(`[Dev] overlay download: phase=${phase}`)
  }, [appVersion])

  // ── Update check / download handlers ──
  const checkForUpdate = useCallback(async () => {
    addLog('[update] checking...')
    const cur = appVersion.replace(/^v/, '')
    // 立即弹窗显示「检查中」— 无论有无更新都弹, 让结果醒目 (不只在 log 里)
    setUpdateInfo({ status: 'checking', current: cur, latest: '', name: '', body: '', url: '' } as any)
    try {
      const info = await hostCall('check_update')
      if (info?.has_update) {
        setUpdateInfo({
          status: 'update',
          current: info.current,
          latest: info.latest,
          name: info.name || '',
          body: info.body || '',
          url: '', // Phase 2: multi-file diff replaces single URL
          diff: info.diff || [],
          message: info.message || '',
          jump_pad: info.jump_pad || '',
          mandatory: !!info.mandatory,
          mode: info.mode || 'incremental',
          staging_state: info.staging_state || undefined,
        } as any)
        const fileCount = info.diff?.length || 0
        addLog(`[update] v${info.latest} available (${fileCount} files, ${info.mode || 'incremental'})`)
      } else if (info?.needs_full_installer) {
        // Manifest schema newer than this client understands — point at full package.
        setUpdateInfo({
          status: 'error', current: info.current || cur, latest: '', name: '', body: '', url: '',
          error: t('update.needs_full_installer'),
        } as any)
        addLog(`[update] full reinstall required — download: ${info.download_url || 'Gitee releases'}`)
      } else if (info?.ok === false) {
        setUpdateInfo({
          status: 'error', current: info.current || cur, latest: '', name: '', body: '', url: '',
          error: info.error || t('update.unknown_error'),
        } as any)
        addLog(`[update] check failed: ${info?.error || 'unknown'}`)
      } else {
        // 已是最新 — 也弹窗 (醒目) 而非只 log
        setUpdateInfo({
          status: 'latest', current: info?.current || cur, latest: info?.current || cur,
          name: '', body: '', url: '',
        } as any)
        addLog(`[update] already latest (v${info?.current || '?'})`)
      }
    } catch (e: any) {
      setUpdateInfo({
        status: 'error', current: cur, latest: '', name: '', body: '', url: '',
        error: e?.message || String(e),
      } as any)
      addLog(`[update] check error: ${e?.message || e}`)
    }
  }, [appVersion, t])

  // Kick off a download for the given diff. C++ returns immediately and drives
  // the UI via update_progress pushes; the app exits when the updater launches.
  const startDownload = useCallback(async (diff: any[]) => {
    if (!diff?.length) { addLog('[update] no files to download'); return }
    setUpdateProgress(null)
    setUpdateDownloading(true)
    addLog(`[update] downloading ${diff.length} files...`)
    try {
      const res = await hostCall('download_update', { diff })
      if (res?.ok === false) {
        addLog(`[update] start failed: ${res.error}`)
        setUpdateDownloading(false)
      }
      // On ok: progress pushes drive the UI; the app exits when updater launches.
    } catch (e: any) {
      addLog(`[update] download error: ${e?.message || e}`)
      setUpdateDownloading(false)
    }
  }, [])

  const downloadUpdate = useCallback(() => {
    if (!updateInfo) return
    if (updateInfo._dev) { addLog('[update] unexpected _dev on SSOT — skipped'); return }
    startDownload(updateInfo.diff || [])
  }, [updateInfo, startDownload])

  // Clear staging dir — user opts to "重新下载" instead of resuming.
  const clearStagingAndDownload = useCallback(async () => {
    if (!updateInfo) return
    if (updateInfo._dev) { addLog('[update] unexpected _dev on SSOT — skipped'); return }
    await hostCall('clear_staging').catch(() => {})
    startDownload(updateInfo.diff || [])
  }, [updateInfo, startDownload])

  // Full update: re-query with force_full to get the complete file set, then download.
  const forceFullUpdate = useCallback(async () => {
    if (updateInfo?._dev) { addLog('[update] unexpected _dev on SSOT — skipped'); return }
    addLog('[update] fetching full package...')
    try {
      const info = await hostCall('check_update', { force_full: true })
      if (info?.has_update) {
        setUpdateInfo({
          current: info.current, latest: info.latest,
          name: info.name || '', body: info.body || '', url: '',
          diff: info.diff || [],
          message: info.message || '', jump_pad: info.jump_pad || '', mandatory: !!info.mandatory, mode: info.mode || 'full',
        } as any)
        startDownload(info.diff || [])
      } else {
        addLog('[update] nothing to full-update')
      }
    } catch (e: any) {
      addLog(`[update] full update error: ${e?.message || e}`)
    }
  }, [startDownload])

  // Subscribe to C++ download progress pushes.
  useEffect(() => onUpdateProgress((m) => {
    setUpdateProgress(m)
    if (m.phase === 'done') addLog('[update] download complete, launching updater...')
    else if (m.phase === 'error') {
      addLog(`[update] failed: ${m.error_file || m.file}`)
      setUpdateDownloading(false)
    }
  }), [])

  useEffect(() => {
    logMgr.initSync()
  }, [])

  // Gate auto-save until disk settings are applied (prevents default wipe on startup).
  // If C++ already injected __BOOT_SETTINGS__, state was initialized from disk — safe to save.
  const [settingsReady, setSettingsReady] = useState(() => hasBootSettings())

  // Load saved settings on startup (from %LOCALAPPDATA% — survives app updates).
  // When boot injection already applied visuals, this is a consistency sync only.
  useEffect(() => {
    hostCall('get_settings').then((res: any) => {
      const s = res?.settings
      if (s && typeof s === 'object') {
        if (s.theme === 'light' || s.theme === 'dark' || s.theme === 'system') setTheme(s.theme)
        if (s.mouseMode === 'seize' || s.mouseMode === 'semi' || s.mouseMode === 'background') setMouseMode(s.mouseMode)
        if (s.keyMode === 'seize' || s.keyMode === 'postmsg' || s.keyMode === 'sendmsg') setKeyMode(s.keyMode)
        if (typeof s.mappingHotkey === 'string' && s.mappingHotkey) setMappingHotkey(s.mappingHotkey)
        if (typeof s.devMode === 'boolean') setDevMode(s.devMode)
        if (s.selfTargetMode === 'warn' || s.selfTargetMode === 'exclude') setSelfTargetMode(s.selfTargetMode)
        if (typeof s.keepFiles === 'number' && s.keepFiles > 0) setKeepFiles(s.keepFiles)
        if (typeof s.autoSnap === 'boolean') setAutoSnap(s.autoSnap)
        if (typeof s.autoStream === 'boolean') setAutoStream(s.autoStream)
        if (typeof s.snapMethod === 'string' && s.snapMethod) setSnapMethod(s.snapMethod)
        if (typeof s.streamMethod === 'string' && s.streamMethod) setStreamMethod(s.streamMethod)
        if (typeof s.renderMethod === 'string' && s.renderMethod) setRenderMethod(s.renderMethod)
        if (typeof s.normalAccent === 'string' && s.normalAccent) setNormalAccentState(s.normalAccent)
        if (typeof s.normalSecondaryAccent === 'string' && s.normalSecondaryAccent) setNormalSecondaryAccentState(s.normalSecondaryAccent)
        if (typeof s.devAccent === 'string' && s.devAccent) setDevAccentState(s.devAccent)
        if (typeof s.devSecondaryAccent === 'string' && s.devSecondaryAccent) setDevSecondaryAccentState(s.devSecondaryAccent)
        if (typeof s.locale === 'string' && s.locale) {
          setLocaleState(s.locale)
          i18n.changeLanguage(s.locale)
          setSavedLocale(s.locale)
        }
        addLog('[settings] loaded')
      }
    }).catch(() => {
      addLog('[settings] load failed — using defaults')
    }).finally(() => {
      setSettingsReady(true)
    })
  }, [])

  // Auto-save settings on change (debounced) — declared early, effect wired after all states below
  const saveTimeout = useRef<ReturnType<typeof setTimeout> | null>(null)

  // ── Initial layout check — auto-collapse if panels overflow container ──
  useEffect(() => {
    const check = () => {
      measureLayout()
      const el = rightPanelRef.current
      if (!el) return
      const kids = el.querySelectorAll(':scope > div')
      let kidsH = 0
      for (let i = 0; i < 3; i++) kidsH += (kids[i] as HTMLElement).offsetHeight
      const overflow = kidsH + GAP - el.clientHeight
      if (overflow > 4) {
        if (logExpandedRef.current && logPinLocked.current === null) {
          addLog(`[Layout] init overflow ${overflow}px → auto-collapse log`)
          setLogExpanded(false)
          setTimeout(check, 250)
        } else if (connectionExpandedRef.current && connPinLocked.current === null) {
          addLog(`[Layout] init overflow ${overflow}px → auto-collapse connection`)
          setConnectionExpanded(false)
          setTimeout(check, 250)
        } else if (
          screenshotExpandedRef.current &&
          ssPinLocked.current === null &&
          !ssHasContentRef.current
        ) {
          addLog(`[Layout] init overflow ${overflow}px → auto-collapse screenshot`)
          setScreenshotExpanded(false)
          setTimeout(check, 250)
        } else if (
          screenshotExpandedRef.current &&
          ssPinLocked.current === null &&
          ssHasContentRef.current
        ) {
          addLog(
            `[Layout] init overflow ${overflow}px → auto-collapse screenshot (has content)`,
          )
          setScreenshotExpanded(false)
        }
      }
      prevClientH.current = el.clientHeight
    }
    const t = setTimeout(check, 400)
    return () => clearTimeout(t)
  }, [])

  // ── Window resize → auto-collapse/expand panels ──
  useEffect(() => {
    const onResize = () => {
      const el = rightPanelRef.current
      if (!el) return
      const ch = el.clientHeight
      if (prevClientH.current === 0) {
        prevClientH.current = ch
        return
      }
      const kids = el.querySelectorAll(':scope > div')
      let kidsH = 0
      for (let i = 0; i < 3; i++) kidsH += (kids[i] as HTMLElement).offsetHeight
      const overflow = kidsH + GAP - ch
      const prev = prevClientH.current
      const now = Date.now()
      const h = H.current

      if (ch < prev) {
        if (overflow > 4) {
          if (
            logExpandedRef.current &&
            logPinLocked.current === null &&
            now - guard.current.L > 350
          ) {
            addLog(`[Layout] overflow ${overflow}px → auto-collapse log`)
            setLogExpanded(false)
            guard.current.L = now
          } else if (
            connectionExpandedRef.current &&
            connPinLocked.current === null &&
            now - guard.current.C > 350
          ) {
            addLog(`[Layout] overflow ${overflow}px → auto-collapse connection`)
            setConnectionExpanded(false)
            guard.current.C = now
          } else if (
            screenshotExpandedRef.current &&
            ssPinLocked.current === null &&
            !ssHasContentRef.current &&
            now - guard.current.S > 350
          ) {
            addLog(`[Layout] overflow ${overflow}px → auto-collapse screenshot`)
            setScreenshotExpanded(false)
            guard.current.S = now
          } else if (
            screenshotExpandedRef.current &&
            ssPinLocked.current === null &&
            ssHasContentRef.current &&
            now - guard.current.S > 350
          ) {
            addLog(
              `[Layout] overflow ${overflow}px → auto-collapse screenshot (has content)`,
            )
            setScreenshotExpanded(false)
            guard.current.S = now
          }
        }
      } else if (ch > prev) {
        if (
          !screenshotExpandedRef.current &&
          ssPinLocked.current === null &&
          ssHasContentRef.current &&
          now - guard.current.S > 350
        ) {
          const wouldNeed = kidsH - h.Sp + h.S + GAP
          if (ch >= wouldNeed) {
            addLog(
              `[Layout] room for S (has content, need ${wouldNeed}px) → auto-expand screenshot`,
            )
            setScreenshotExpanded(true)
            guard.current.S = now
          }
        } else if (
          !connectionExpandedRef.current &&
          connPinLocked.current === null &&
          now - guard.current.C > 350
        ) {
          const wouldNeed = kidsH - h.Cp + h.C + GAP
          if (ch >= wouldNeed) {
            addLog(`[Layout] room for C (need ${wouldNeed}px) → auto-expand connection`)
            setConnectionExpanded(true)
            guard.current.C = now
          }
        } else if (
          !screenshotExpandedRef.current &&
          ssPinLocked.current === null &&
          !ssHasContentRef.current &&
          now - guard.current.S > 350
        ) {
          const wouldNeed = kidsH - h.Sp + h.S + GAP
          if (ch >= wouldNeed) {
            addLog(`[Layout] room for S (need ${wouldNeed}px) → auto-expand screenshot`)
            setScreenshotExpanded(true)
            guard.current.S = now
          }
        } else if (
          !logExpandedRef.current &&
          logPinLocked.current === null &&
          now - guard.current.L > 350
        ) {
          const wouldNeed = kidsH - h.Lp + h.L + GAP
          if (ch >= wouldNeed) {
            addLog(`[Layout] room for L (need ${wouldNeed}px) → auto-expand log`)
            setLogExpanded(true)
            guard.current.L = now
          }
        }
      }
      prevClientH.current = ch
    }
    window.addEventListener('resize', onResize)
    return () => window.removeEventListener('resize', onResize)
  }, [])

  // ── Drag-end overflow check — same logic as resize but called after divider drag ──
  const checkVerticalLayout = useCallback(() => {
    const el = rightPanelRef.current
    if (!el) return
    const kids = el.querySelectorAll(':scope > div')
    if (kids.length < 3) return
    let kidsH = 0
    for (let i = 0; i < 3; i++) kidsH += (kids[i] as HTMLElement).offsetHeight
    const ch = el.clientHeight
    const h = H.current
    const overflow = kidsH + GAP - ch

    if (overflow > 4) {
      if (logExpandedRef.current && logPinLocked.current === null) {
        addLog(`[Layout] drag overflow ${overflow}px → auto-collapse log`)
        setLogExpanded(false)
      } else if (connectionExpandedRef.current && connPinLocked.current === null) {
        addLog(`[Layout] drag overflow ${overflow}px → auto-collapse connection`)
        setConnectionExpanded(false)
      } else if (
        screenshotExpandedRef.current &&
        ssPinLocked.current === null &&
        !ssHasContentRef.current
      ) {
        addLog(`[Layout] drag overflow ${overflow}px → auto-collapse screenshot`)
        setScreenshotExpanded(false)
      } else if (
        screenshotExpandedRef.current &&
        ssPinLocked.current === null &&
        ssHasContentRef.current
      ) {
        addLog(
          `[Layout] drag overflow ${overflow}px → auto-collapse screenshot (has content)`,
        )
        setScreenshotExpanded(false)
      }
    } else if (overflow < -4) {
      if (
        !screenshotExpandedRef.current &&
        ssPinLocked.current === null &&
        ssHasContentRef.current
      ) {
        const wouldNeed = kidsH - h.Sp + h.S + GAP
        if (ch >= wouldNeed) {
          addLog(
            `[Layout] drag room for S (has content, need ${wouldNeed}px) → auto-expand screenshot`,
          )
          setScreenshotExpanded(true)
        }
      } else if (
        !connectionExpandedRef.current &&
        connPinLocked.current === null
      ) {
        const wouldNeed = kidsH - h.Cp + h.C + GAP
        if (ch >= wouldNeed) {
          addLog(
            `[Layout] drag room for C (need ${wouldNeed}px) → auto-expand connection`,
          )
          setConnectionExpanded(true)
        }
      } else if (
        !screenshotExpandedRef.current &&
        ssPinLocked.current === null &&
        !ssHasContentRef.current
      ) {
        const wouldNeed = kidsH - h.Sp + h.S + GAP
        if (ch >= wouldNeed) {
          addLog(`[Layout] drag room for S (need ${wouldNeed}px) → auto-expand screenshot`)
          setScreenshotExpanded(true)
        }
      } else if (!logExpandedRef.current && logPinLocked.current === null) {
        const wouldNeed = kidsH - h.Lp + h.L + GAP
        if (ch >= wouldNeed) {
          addLog(`[Layout] drag room for L (need ${wouldNeed}px) → auto-expand log`)
          setLogExpanded(true)
        }
      }
    }
  }, [])

  // ── Horizontal auto-collapse — when window too narrow for both panels ──
  const H_COLLAPSE_THRESHOLD = MIN_LEFT_WIDTH + DEFAULT_RIGHT_WIDTH + 24
  const autoCollapsedByWidth = useRef(false)

  useEffect(() => {
    const onResize = () => {
      const w = window.innerWidth
      if (w < H_COLLAPSE_THRESHOLD && !rightCollapsed) {
        autoCollapsedByWidth.current = true
        setRightCollapsed(true)
        addLog('[Layout] window too narrow → auto-collapse right panel')
      } else if (w >= H_COLLAPSE_THRESHOLD && rightCollapsed && autoCollapsedByWidth.current) {
        autoCollapsedByWidth.current = false
        setRightCollapsed(false)
        addLog('[Layout] window wide enough → auto-expand right panel')
      }
    }
    window.addEventListener('resize', onResize)
    onResize()
    return () => window.removeEventListener('resize', onResize)
  }, [rightCollapsed])

  // ═══ Capture state ═══
  const isResizing = useRef(false)
  // ── Selected target window ──
  const [selWindow, setSelWindow] = useState<WindowInfo>({
    title: DESKTOP_TITLE,
    category: 'desktop',
    hwnd: 0,
  })
  const [screenRatio, setScreenRatio] = useState(16 / 9)

  // ── Capture method config (snapshot + stream independent) ──
  const [snapMethod, setSnapMethod] = useState(
    () => (typeof boot.snapMethod === 'string' && boot.snapMethod ? boot.snapMethod : 'dxgi'),
  )
  const [streamMethod, setStreamMethod] = useState(
    () => (typeof boot.streamMethod === 'string' && boot.streamMethod ? boot.streamMethod : 'wgc'),
  )
  const [autoSnap, setAutoSnap] = useState(() => (typeof boot.autoSnap === 'boolean' ? boot.autoSnap : true))
  const [autoStream, setAutoStream] = useState(() => (typeof boot.autoStream === 'boolean' ? boot.autoStream : true))
  const [renderMethod, setRenderMethod] = useState(
    () => (typeof boot.renderMethod === 'string' && boot.renderMethod ? boot.renderMethod : 'shared'),
  )
  const [expectedCaptureState, setExpectedCaptureState] = useState('desktop')

  // ── General settings ──
  const [keepFiles, setKeepFiles] = useState(() => (typeof boot.keepFiles === 'number' && boot.keepFiles > 0 ? boot.keepFiles : 5))
  const [mouseMode, setMouseMode] = useState<'seize' | 'semi' | 'background'>(
    () => (boot.mouseMode === 'seize' || boot.mouseMode === 'semi' || boot.mouseMode === 'background' ? boot.mouseMode : 'background'),
  )
  const [keyMode, setKeyMode] = useState<'seize' | 'postmsg' | 'sendmsg'>(
    () => (boot.keyMode === 'seize' || boot.keyMode === 'postmsg' || boot.keyMode === 'sendmsg' ? boot.keyMode : 'postmsg'),
  )
  const [mappingEnabled, setMappingEnabled] = useState(false)
  const [mappingHotkey, setMappingHotkey] = useState(
    () => (typeof boot.mappingHotkey === 'string' && boot.mappingHotkey ? boot.mappingHotkey : 'F10'),
  )

  // ── Dev mode + accent colors ──
  const [devMode, setDevMode] = useState(() => (typeof boot.devMode === 'boolean' ? boot.devMode : false))
  const [normalAccent, setNormalAccentState] = useState(
    () => (typeof boot.normalAccent === 'string' && boot.normalAccent ? boot.normalAccent : '#3B82F6'),
  )
  const [normalSecondaryAccent, setNormalSecondaryAccentState] = useState(
    () => (typeof boot.normalSecondaryAccent === 'string' && boot.normalSecondaryAccent ? boot.normalSecondaryAccent : '#F97316'),
  )
  const [devAccent, setDevAccentState] = useState(
    () => (typeof boot.devAccent === 'string' && boot.devAccent ? boot.devAccent : '#EF4444'),
  )
  const [devSecondaryAccent, setDevSecondaryAccentState] = useState(
    () => (typeof boot.devSecondaryAccent === 'string' && boot.devSecondaryAccent ? boot.devSecondaryAccent : '#22C55E'),
  )
  // Derived: current accent depends on devMode
  const accent = devMode ? devAccent : normalAccent
  const secondaryAccent = devMode ? devSecondaryAccent : normalSecondaryAccent
  // Sync CSS variables whenever accent changes
  useEffect(() => {
    document.documentElement.style.setProperty('--color-accent', accent)
    document.documentElement.style.setProperty('--color-accent-secondary', secondaryAccent)
  }, [accent, secondaryAccent])
  // Frame dump
  const [saveCaptureFrames, setSaveCaptureFrames] = useState(false)
  const [saveStreamFrames, setSaveStreamFrames] = useState(false)
  const [frameDumpDir, setFrameDumpDir] = useState('')

  // ── Target window state (polled every 500ms) ──
  const [winState, setWinState] = useState('desktop')
  const lastWinStateRef = useRef('desktop')

  // ── Capture operation state machine ──
  // idle → snapshotting → idle  (single frame, auto-transitions back)
  // idle → streaming → idle     (continuous, stays until stopStream)
  const opStateRef = useRef<'idle' | 'snapshotting' | 'streaming'>('idle')
  const snapCancelRef = useRef(0)             // incremented to cancel stale snapshots
  const [previewing, setPreviewing] = useState(false)
  const previewingRef = useRef(false)          // ref version for SharedBuffer handler closure
  const snapshotRef = useRef(false)            // true while waiting for snapshot frame
  const snapshotStartRef = useRef(0)           // Date.now() when snapshot was triggered
  const [capMethod, setCapMethod] = useState('')        // actual method used (from C++ response)
  const [snapshotLatency, setSnapshotLatency] = useState<number | null>(null)
  const [streamFps, setStreamFps] = useState(0)
  const [targetDims, setTargetDims] = useState<{w:number,h:number} | null>(null)
  const [agentConnectedReal, setAgentConnectedReal] = useState(false) // SSOT — TCP :9999 clients
  // display = overlay ?? real (computed below after demo state)

  // ── Self-target detection: GAM window rect + virtual screen rect ──
  const [selfRect, setSelfRect] = useState<Rect | null>(null)
  const [screenRect, setScreenRect] = useState<Rect | null>(null)
  const [selfTargetMode, setSelfTargetMode] = useState<'warn' | 'exclude'>(
    () => (boot.selfTargetMode === 'warn' || boot.selfTargetMode === 'exclude' ? boot.selfTargetMode : 'warn'),
  )

  // ── Auto-save settings on change (debounced 1s, single atomic write) ──
  // One set_settings call avoids the old concurrent per-key RMW that corrupted settings.json.
  useEffect(() => {
    if (!settingsReady) return
    if (saveTimeout.current) clearTimeout(saveTimeout.current)
    saveTimeout.current = setTimeout(() => {
      hostCall('set_settings', {
        settings: {
          theme,
          mouseMode,
          keyMode,
          mappingHotkey,
          devMode,
          selfTargetMode,
          keepFiles,
          autoSnap,
          autoStream,
          snapMethod,
          streamMethod,
          renderMethod,
          normalAccent,
          normalSecondaryAccent,
          devAccent,
          devSecondaryAccent,
          locale,
        },
      }).catch(() => {})
    }, 1000)
    return () => { if (saveTimeout.current) clearTimeout(saveTimeout.current) }
  }, [settingsReady, theme, mouseMode, keyMode, mappingHotkey, devMode, selfTargetMode,
      keepFiles, autoSnap, autoStream, snapMethod, streamMethod, renderMethod,
      normalAccent, normalSecondaryAccent, devAccent, devSecondaryAccent, locale])

  // ── Load screen info for aspect ratio + self-target detection ──
  useEffect(() => {
    ;(async () => {
      try {
        const si = await hostCall('screen_info')
        setScreenRatio(si.w / si.h)
        setScreenRect({ x: si.x || 0, y: si.y || 0, w: si.w, h: si.h })
      } catch (_) {}
    })()
    // Poll GAM self-rect every 2s (window may be moved/resized)
    const pollSelf = async () => {
      try {
        const r = await hostCall('get_self_rect')
        if (r && r.w > 0 && r.h > 0) setSelfRect(r)
      } catch (_) {}
    }
    pollSelf()
    const iv = setInterval(pollSelf, 2000)
    return () => clearInterval(iv)
  }, [])

  // ── Sync self-target mode → C++ set_exclude_self ──
  const syncingMode = useRef(false)
  useEffect(() => {
    if (syncingMode.current) return
    syncingMode.current = true
    const exclude = selfTargetMode === 'exclude'
    hostCall('set_exclude_self', { exclude })
      .then((res: any) => {
        if (!res?.ok && exclude) {
          addLog(`[SelfTarget] exclude unsupported on this system — using warn mode`)
          setSelfTargetMode('warn')
        }
      })
      .catch(() => { if (exclude) setSelfTargetMode('warn') })
      .finally(() => { syncingMode.current = false })
  }, [selfTargetMode])

  // ── Poll window state every 500ms (foreground/background/minimized) ──
  useEffect(() => {
    const poll = async () => {
      try {
        const s = await hostCall('window_state', { hwnd: selWindow.hwnd })
        if (s !== lastWinStateRef.current) {
          lastWinStateRef.current = s
          setWinState(s)
        }
      } catch (_) {}
    }
    poll()
    const iv = setInterval(poll, 500)
    return () => clearInterval(iv)
  }, [selWindow.hwnd])

  // ── Auto-select capture methods based on window state ──
  // Desktop/minimized → DXGI; foreground/background window → WGC
  useEffect(() => {
    const isDesktop = selWindow.hwnd === 0
    if (autoSnap) {
      setSnapMethod(isDesktop || winState === 'minimized' ? 'dxgi' : 'wgc')
    }
    if (autoStream) {
      setStreamMethod('wgc')
    }
  }, [selWindow.hwnd, winState, autoSnap, autoStream])

  // ═══ Capture operations ═══
  // ── Stop stream → idle ──
  const stopStream = useCallback(async () => {
    if (opStateRef.current !== 'streaming') return
    previewingRef.current = false
    setPreviewing(false)
    try {
      await hostCall('capture_stream_stop')
    } catch (_) {}
    opStateRef.current = 'idle'
    addLog('[Capture] stream stopped')
    setSsExpanded(false)
  }, [])

  // ── Take single snapshot ──
  // Auto-stops stream if running. Guards against re-entry and minimized+cantCapture.
  const takeSnapshot = useCallback(async () => {
    if (opStateRef.current === 'streaming') {
      addLog('[Capture] auto-stop stream before snapshot')
      await stopStream()
    }
    if (opStateRef.current === 'snapshotting') {
      addLog('[Capture] snapshot already in progress, ignoring')
      return
    }
    opStateRef.current = 'snapshotting'
    const snapId = ++snapCancelRef.current
    const hwnd = selWindow.hwnd ?? 0
    const method = snapMethod

    if (cantCaptureMinimized(method, winState)) {
      addLog(`[Capture] blocked: window minimized, ${method} cannot capture`)
      opStateRef.current = 'idle'
      return
    }
    if (!screenshotExpanded) setSsExpanded(true)
    const t0 = Date.now()
    snapshotStartRef.current = t0
    snapshotRef.current = true
    setSnapshotLatency(null)
    try {
      const info = (await hostCall('capture_window', { hwnd, method })) as {
        ok?: boolean
        method?: string
        w?: number
        h?: number
      }
      const elapsed = Date.now() - t0
      if (snapId !== snapCancelRef.current) {
        addLog('[Capture] snapshot cancelled by later operation')
        return
      }
      if (info && info.ok) {
        if (info.method) setCapMethod(info.method)
        addLog(`[Capture] OK ${info.w}x${info.h} (${elapsed}ms) [${info.method || '?'}]`)
        setSnapshotLatency(elapsed)
      } else {
        snapshotRef.current = false
        addLog(`[Capture] failed (${elapsed}ms)`)
      }
    } catch (ex: any) {
      if (snapId !== snapCancelRef.current) return
      snapshotRef.current = false
      addLog(`[Capture] EXCEPTION: ${ex?.message || ex} after ${Date.now() - t0}ms`)
    }
    opStateRef.current = 'idle'
  }, [selWindow.hwnd, snapMethod, winState, stopStream])

  // ── Start live preview stream ──
  // Cancels any in-flight snapshot (increments snapCancelRef).
  const startStream = useCallback(async () => {
    ++snapCancelRef.current
    snapshotRef.current = false

    if (opStateRef.current === 'streaming') {
      addLog('[Capture] stream already starting, ignoring')
      return
    }

    const hwnd = selWindow.hwnd ?? 0
    if (cantCaptureMinimized(streamMethod, winState)) {
      addLog(`[Preview] blocked: window minimized, ${streamMethod} cannot capture`)
      return
    }

    opStateRef.current = 'streaming'
    previewingRef.current = true
    setPreviewing(true)
    setCapMethod(streamMethod)
    setSnapshotLatency(null)

    try {
      await hostCall('capture_stream_start', {
        hwnd,
        tcpPort: 9999,
        method: streamMethod,
        transport: renderMethod,
      })
    } catch (e) {
      previewingRef.current = false
      setPreviewing(false)
      opStateRef.current = 'idle'
      addLog(`[Preview] start failed: ${e}`)
      return
    }
    if (!screenshotExpanded) setSsExpanded(true)
  }, [selWindow.hwnd, streamMethod, winState, renderMethod, screenshotExpanded])

  // ── Toggle preview on/off ──
  const togglePreview = useCallback(async () => {
    if (previewing) {
      await stopStream()
    } else {
      await startStream()
    }
  }, [previewing, stopStream, startStream])

  // ═══ Self-Test orchestration ═══
  // One-click calibration: reuses the real user-facing callbacks (select →
  // preview → mapping → click) end-to-end, then compares test_target's TCP
  // feedback against expected landings. Step 1 (launch + connect) is the only
  // genuinely new logic; steps 2–4 drive existing handlers.
  const monitorApiRef = useRef<MonitorApi | null>(null)
  const selfTestAbort = useRef(false)
  const [selfTest, setSelfTest] = useState<SelfTestState>({ phase: 'idle' })
  // keep latest capture callback reachable from the async flow (avoid stale closure)
  const startStreamRef = useRef(startStream)
  startStreamRef.current = startStream

  // Leave Dev mode — 企业级三步：清 overlay → 关 Dev 能力 → 重检 SSOT
  const selfTestLiveRef = useRef(selfTest)
  selfTestLiveRef.current = selfTest
  const frameDumpRef = useRef({ capture: false, stream: false, dir: '' })
  frameDumpRef.current = { capture: saveCaptureFrames, stream: saveStreamFrames, dir: frameDumpDir }

  const refreshAgentStatus = useCallback(async () => {
    try {
      const r = await hostCall('get_agent_status')
      setAgentConnectedReal(!!r?.connected)
    } catch {
      setAgentConnectedReal(false)
    }
  }, [])

  // Startup + periodic truth (SSOT); overlay never touches this
  useEffect(() => {
    refreshAgentStatus()
    const id = window.setInterval(() => { refreshAgentStatus() }, 3000)
    return () => window.clearInterval(id)
  }, [refreshAgentStatus])

  const leaveDevModeCleanup = useCallback(() => {
    // ① Clear UI overlays (Demo 层)
    setDemoAgentOverride(null)
    setDemoUpdateInfo(null)
    setDemoUpdateDownloading(false)
    setDemoUpdateProgress(null)
    setDemoSelfTest(null)

    // ② Disable Dev capabilities (Android-style)
    const st = selfTestLiveRef.current
    if (st.phase === 'running') {
      selfTestAbort.current = true
      hostCall('selftest_disconnect').catch(() => {})
      setSelfTest({ phase: 'idle' })
    }
    const dump = frameDumpRef.current
    if (dump.capture || dump.stream) {
      setSaveCaptureFrames(false)
      setSaveStreamFrames(false)
      hostCall('set_frame_dump', { capture: false, stream: false, dir: dump.dir }).catch(() => {})
    }
    hostCall('find_test_target')
      .then((r: any) => { if (r?.hwnd) return hostCall('launch_test_target') })
      .catch(() => {})

    setTab((t) => (t === 'DevTools' ? 'Settings' : t))

    // ③ Re-detect from SSOT
    refreshAgentStatus().then(() => {
      addLog('[Dev] left dev mode — overlays cleared, caps off, SSOT refreshed')
    })
  }, [refreshAgentStatus])

  const setDevModeSafe = useCallback((on: boolean) => {
    if (!on) leaveDevModeCleanup()
    setDevMode(on)
  }, [leaveDevModeCleanup])

  const closeTestTargetAfterSelfTest = useCallback(async () => {
    // Self-test owns the lifecycle: stop preview, then close the window.
    // Manual testing must use DevTools → Test Target → Launch.
    try {
      if (previewingRef.current) await stopStream()
      setMappingEnabled(false)
      const r: any = await hostCall('find_test_target')
      if (r?.hwnd) {
        await hostCall('launch_test_target') // toggle → close
        addLog('[SelfTest] test_target closed (use DevTools Launch for manual tests)')
      }
    } catch (e: any) {
      addLog(`[SelfTest] close test_target failed: ${e?.message || e}`)
    }
  }, [stopStream, setMappingEnabled])

  const runSelfTestFlow = useCallback(
    async (perCell: number) => {
      if (selfTest.phase === 'running') return
      selfTestAbort.current = false
      setSelfTest({ phase: 'running', done: 0, total: 0 })
      addLog(`[SelfTest] start (perCell=${perCell})`)
      try {
        // 1 — ensure the test_target window is running (never toggle-close it here)
        let hwnd = (await hostCall('find_test_target'))?.hwnd || 0
        if (!hwnd) {
          await hostCall('launch_test_target')
          for (let i = 0; i < 40 && !hwnd; i++) {
            await sleep(100)
            hwnd = (await hostCall('find_test_target'))?.hwnd || 0
          }
        }
        if (!hwnd) throw new Error('test_target 窗口未找到')
        // TCP server starts with the HWND; give accept() a moment before connect.
        await sleep(300)

        // 2 — select it as the capture target (same state a user selection sets)
        if (opStateRef.current === 'streaming') await stopStream()
        setSelWindow({ title: 'GAM Test Target', category: 'window', hwnd })
        setTab('Monitor')
        await sleep(250) // let React re-render → fresh capture closures + MonitorView mount

        // 3 — preview + mapping (reuse the real preview start + mapping toggle)
        if (!previewingRef.current) await startStreamRef.current()
        setMappingEnabled(true)
        for (let i = 0; i < 80; i++) {
          if (monitorApiRef.current?.ready()) break
          await sleep(100)
        }
        if (!monitorApiRef.current?.ready()) throw new Error('预览/映射未就绪')

        // 4 — dense sweep + interaction scenarios (real send_input paths)
        const api = monitorApiRef.current!
        const summary = await runSelfTest({
          perCell,
          api: {
            sendClick: (rx, ry, b) => api.sendClick(rx, ry, b),
            sendWheel: (rx, ry, d) => api.sendWheel(rx, ry, d),
            sendDrag: (path, b) => api.sendDrag(path, b),
            sendText: (text) => api.sendText(text),
            sendKey: (type, key, code, vk) => api.sendKey(type, key, code, vk),
          },
          onProgress: (done, total, step) =>
            setSelfTest((s) => (s.phase === 'running' ? { ...s, done, total, step } : s)),
          shouldAbort: () => selfTestAbort.current,
        })
        setSelfTest({ phase: 'done', summary })
        const scenOk = summary.scenarios.filter((x) => x.ok).length
        const pct = (n: number, d: number) => (d > 0 ? Math.round((n / d) * 100) : 0)
        addLog(
          `[SelfTest] done recv=${summary.received}/${summary.total} (${pct(summary.received, summary.total)}%) ` +
          `cell=${pct(summary.cellMatch, summary.total)}% hit=${pct(summary.hitMatch, summary.total)}% ` +
          `scen=${scenOk}/${summary.scenarios.length} ` +
          `off=(${summary.meanDx.toFixed(1)},${summary.meanDy.toFixed(1)}) ` +
          `err=${summary.meanAbs.toFixed(1)}/${summary.maxAbs.toFixed(1)}` +
          (summary.aborted ? ' ABORTED' : ''),
        )
        for (const sc of summary.scenarios) {
          addLog(`[SelfTest] scenario ${sc.ok ? 'OK' : 'FAIL'} ${sc.id}: ${sc.detail}`)
        }
        // Compact per-row heatmap line for log history (no need to copy the modal).
        if (summary.cells?.length) {
          const rows = summary.cells.map((row, y) =>
            `y${y}=[` + row.map((r) => Math.round(r * 100)).join(',') + ']',
          )
          addLog(`[SelfTest] heatmap ${rows.join(' ')}`)
        }
      } catch (e: any) {
        setSelfTest({ phase: 'error', error: e?.message || String(e) })
        addLog(`[SelfTest] error: ${e?.message || e}`)
      } finally {
        await closeTestTargetAfterSelfTest()
      }
    },
    [selfTest.phase, stopStream, setMappingEnabled, closeTestTargetAfterSelfTest],
  )

  // ── Cleanup on unmount: stop any active stream ──
  useEffect(() => {
    return () => {
      previewingRef.current = false
      snapshotRef.current = false
      hostCall('capture_stream_stop').catch(() => {})
    }
  }, [])

  // ── Resize divider drag handler ──
  const handleResizeStart = useCallback(
    (e: React.MouseEvent) => {
      autoCollapsedByWidth.current = false
      e.preventDefault()
      isResizing.current = true
      document.body.style.cursor = 'col-resize'
      document.body.style.userSelect = 'none'
      const onMove = (ev: MouseEvent) => {
        const w = document.body.clientWidth - ev.clientX
        if (w < 160) setRightCollapsed(true)
        else {
          setRightCollapsed(false)
          setRightWidth(Math.max(324, Math.min(400, w)))
        }
      }
      const onUp = () => {
        isResizing.current = false
        document.body.style.cursor = ''
        document.body.style.userSelect = ''
        window.removeEventListener('mousemove', onMove)
        window.removeEventListener('mouseup', onUp)
        addLog('[Layout] right panel resized')
        requestAnimationFrame(() => {
          measureLayout()
          checkVerticalLayout()
        })
      }
      window.addEventListener('mousemove', onMove)
      window.addEventListener('mouseup', onUp)
    },
    [measureLayout, checkVerticalLayout],
  )

  // ═══ Display = overlay ?? SSOT (企业级；关 Dev 清 overlay 后自动回到真相) ═══
  const displayAgentConnected = demoAgentOverride ?? agentConnectedReal
  const displayUpdateInfo = demoUpdateInfo ?? updateInfo
  const displayUpdateDownloading = demoUpdateInfo ? demoUpdateDownloading : updateDownloading
  const displayUpdateProgress = demoUpdateInfo ? demoUpdateProgress : updateProgress
  const displaySelfTest = demoSelfTest ?? selfTest
  const displayHasUpdate = displayUpdateInfo?.status === 'update'

  // ═══ Render ═══
  return (
    <div className="relative h-full flex flex-col bg-bg-primary">
      {/* Startup skeleton overlay — covers the UI (z-50) until initial init settles */}
      {(!appReady || previewSkeleton) && <LoadingScreen />}
      {/* ── Top bar: tabs + Start/Stop + theme ── */}
      <TopBar
        tab={tab}
        setTab={setTab}
        running={running}
        onStart={() => setRunning(true)}
        onStop={() => setRunning(false)}
        dark={resolvedDark}
        onToggleTheme={() => setTheme(resolvedDark ? 'light' : 'dark')}
        devMode={devMode}
        locale={locale}
        setLocale={setLocale}
        isAdmin={isAdmin}
        onSwitchPermission={switchPermission}
      />
      {/* ── Main content area: left (tab content) + resize divider + right (panels) ── */}
      <div className="flex-1 flex overflow-hidden">
        {/* ── Left: tab content (Settings / Monitor / Log) ── */}
        <div
          className="flex-1 flex flex-col overflow-hidden border-r border-border"
          style={{ minWidth: MIN_LEFT_WIDTH }}
        >
          {/* Settings tab */}
          {tab === 'Settings' && (
            <SettingsView
              snapMethod={snapMethod} setSnapMethod={setSnapMethod}
              streamMethod={streamMethod} setStreamMethod={setStreamMethod}
              renderMethod={renderMethod} setRenderMethod={setRenderMethod}
              autoSnap={autoSnap} setAutoSnap={setAutoSnap}
              autoStream={autoStream} setAutoStream={setAutoStream}
              normalAccent={normalAccent} setNormalAccentState={setNormalAccentState}
              normalSecondaryAccent={normalSecondaryAccent} setNormalSecondaryAccentState={setNormalSecondaryAccentState}
              devAccent={devAccent} setDevAccentState={setDevAccentState}
              devSecondaryAccent={devSecondaryAccent} setDevSecondaryAccentState={setDevSecondaryAccentState}
              accent={accent} secondaryAccent={secondaryAccent}
              locale={locale} setLocale={setLocale}
              selWin={selWindow} winState={winState}
              expectedCaptureState={expectedCaptureState}
              setExpectedCaptureState={setExpectedCaptureState}
              onSelect={(w: WindowInfo) => {
                if (opStateRef.current === 'streaming') stopStream()
                setSelWindow(w)
              }}
              onDisconnect={() => {
                if (opStateRef.current === 'streaming') stopStream()
                setSelWindow({
                  title: DESKTOP_TITLE,
                  category: 'desktop',
                  hwnd: 0,
                })
                setExpectedCaptureState('desktop')
                addLog('[Connection] disconnected, back to desktop')
              }}
              keepFiles={keepFiles} setKeepFiles={setKeepFiles}
              appVersion={appVersion} theme={theme} setTheme={setTheme}
              devMode={devMode} setDevMode={setDevModeSafe}
              mouseMode={mouseMode} setMouseMode={setMouseMode}
              keyMode={keyMode} setKeyMode={setKeyMode}
              mappingHotkey={mappingHotkey} setMappingHotkey={setMappingHotkey}
              selfTargetMode={selfTargetMode} setSelfTargetMode={setSelfTargetMode}
              onCheckUpdate={checkForUpdate}
              hasUpdate={displayHasUpdate}
              isAdmin={isAdmin}
              onSwitchPermission={switchPermission}
            />
          )}
          {/* Dev tab */}
          {tab === 'DevTools' && (
            <DevToolsView
              appVersion={appVersion}
              saveCaptureFrames={saveCaptureFrames} setSaveCaptureFrames={setSaveCaptureFrames}
              saveStreamFrames={saveStreamFrames} setSaveStreamFrames={setSaveStreamFrames}
              frameDumpDir={frameDumpDir} setFrameDumpDir={setFrameDumpDir}
              onRunSelfTest={runSelfTestFlow}
              selfTestRunning={selfTest.phase === 'running'}
              onPreviewSkeleton={previewSkeletonScreen}
              onDevInjectUpdate={devInjectUpdate}
              onDevInjectDownload={devInjectDownload}
              onDevInjectSelfTest={devInjectSelfTest}
              onDevInjectAgent={devInjectAgent}
            />
          )}
          {/* Monitor tab */}
          {tab === 'Monitor' && (
            <MonitorView
              selWin={selWindow} winState={winState}
              capMethod={capMethod}
              snapMethod={snapMethod} streamMethod={streamMethod}
              previewing={previewing}
              snapshotLatency={snapshotLatency}
              onTakeSnapshot={takeSnapshot}
              onTogglePreview={togglePreview}
              mouseMode={mouseMode}
              keyMode={keyMode}
              mappingEnabled={mappingEnabled} setMappingEnabled={setMappingEnabled}
              mappingHotkey={mappingHotkey}
              targetDims={targetDims}
              selfRect={selfRect}
              screenRect={screenRect}
              selfTargetMode={selfTargetMode}
              apiRef={monitorApiRef}
            >
              <ScreenshotPanel
                selWin={selWindow} screenRatio={screenRatio}
                snapMethod={snapMethod} streamMethod={streamMethod}
                renderMethod={renderMethod} winState={winState}
                expanded={true} onToggle={() => {}}
                previewing={previewing} previewingRef={previewingRef}
                snapshotRef={snapshotRef} snapshotStartRef={snapshotStartRef}
                capMethod={capMethod}
                onTakeSnapshot={takeSnapshot}
                onTogglePreview={togglePreview}
                pinned={false} onTogglePin={() => {}}
                hasContentRef={ssHasContentRef}
                bare
                onFps={setStreamFps}
                onDims={(w, h) => setTargetDims({w, h})}
              />
            </MonitorView>
          )}
          {/* Log tab */}
          {tab === 'Log' && <LogPanel keepFiles={keepFiles} />}
          {/* ── Bottom status strip ── */}
          <BottomBar
            selWin={displayTargetTitle(selWindow.title, t)}
            snapMethod={snapMethod} streamMethod={streamMethod}
            previewing={previewing} fps={streamFps}
            targetDims={targetDims}
            appVersion={appVersion}
            agentConnected={displayAgentConnected}
            hasUpdate={displayHasUpdate}
            onCheckUpdate={checkForUpdate}
          />
        </div>
        {/* ── Resize divider ── */}
        <Tooltip
          text={
            rightCollapsed ? t('app.resize_expand') : t('app.resize_drag')
          }
        >
          <div
            onMouseDown={handleResizeStart}
            className={`${rightCollapsed ? 'w-4' : 'w-1'} hover:bg-accent/50 cursor-col-resize flex items-center justify-center group shrink-0 transition-all select-none bg-transparent`}
          >
            <div className="w-[2px] h-8 rounded-full transition-colors bg-border group-hover:bg-accent" />
          </div>
        </Tooltip>
        {/* ── Right panel stack: Connection → Screenshot → Log → spacer ── */}
        {!rightCollapsed && (
          <div
            ref={rightPanelRef}
            className="flex flex-col p-3 gap-3 overflow-hidden min-h-0"
            style={{ width: rightWidth, minWidth: 324, maxWidth: 400 }}
          >
            {/* Connection panel */}
            <div className="shrink-0">
              <ConnectionPanel
                onSelect={(w: WindowInfo) => {
                  if (opStateRef.current === 'streaming') stopStream()
                  setSelWindow(w)
                }}
                onDisconnect={() => {
                  if (opStateRef.current === 'streaming') stopStream()
                  setSelWindow({
                    title: DESKTOP_TITLE,
                    category: 'desktop',
                    hwnd: 0,
                  })
                  setExpectedCaptureState('desktop')
                  addLog('[Connection] disconnected, back to desktop')
                }}
                snapMethod={snapMethod} setSnapMethod={setSnapMethod}
                streamMethod={streamMethod} setStreamMethod={setStreamMethod}
                selWin={selWindow} winState={winState}
                expectedCaptureState={expectedCaptureState}
                setExpectedCaptureState={setExpectedCaptureState}
                expanded={connectionExpanded}
                onToggle={() => setConnExpanded(!connectionExpandedRef.current)}
                pinned={connectionPinned} onTogglePin={toggleConnPin}
              />
            </div>
            {/* Screenshot panel (overflow-hidden for grid animation) */}
            <div className="shrink-0 overflow-hidden">
              <ScreenshotPanel
                selWin={selWindow} screenRatio={screenRatio}
                snapMethod={snapMethod} streamMethod={streamMethod}
                renderMethod={renderMethod} winState={winState}
                expanded={screenshotExpanded}
                onToggle={() => setSsExpanded(!screenshotExpandedRef.current)}
                previewing={previewing} previewingRef={previewingRef}
                snapshotRef={snapshotRef} snapshotStartRef={snapshotStartRef}
                capMethod={capMethod}
                onTakeSnapshot={takeSnapshot}
                onTogglePreview={togglePreview}
                pinned={screenshotPinned} onTogglePin={toggleSsPin}
                hasContentRef={ssHasContentRef}
                onFps={setStreamFps}
                onDims={(w, h) => setTargetDims({w, h})}
              />
            </div>
            {/* Log panel (compact mode in sidebar) */}
            <div className="shrink-0">
              <LogPanel
                compact expanded={logExpanded}
                onToggle={() => setLogPanelExpanded(!logExpandedRef.current)}
                pinned={logPinned} onTogglePin={toggleLogPin}
              />
            </div>
            {/* Flexible spacer to absorb leftover vertical space */}
            <div className="flex-1" />
          </div>
        )}
      </div>

      {/* ── Self-Test report overlay ── */}
      <SelfTestModal
        state={displaySelfTest}
        onClose={() => {
          if (demoSelfTest) setDemoSelfTest(null)
          else setSelfTest({ phase: 'idle' })
        }}
        onAbort={() => {
          if (demoSelfTest) setDemoSelfTest(null)
          else selfTestAbort.current = true
        }}
      />

      {/* ── Update modal: real SSOT or Dev overlay (no backend for demos) ── */}
      {displayUpdateInfo && (
        <UpdateModal
          info={displayUpdateInfo}
          downloading={displayUpdateDownloading}
          progress={displayUpdateProgress}
          onDownload={() => {
            if (demoUpdateInfo) { addLog('[update] demo overlay — no download'); return }
            downloadUpdate()
          }}
          onClearAndDownload={() => {
            if (demoUpdateInfo) { addLog('[update] demo overlay — no clear/download'); return }
            clearStagingAndDownload()
          }}
          onForceUpdate={() => {
            if (demoUpdateInfo) { addLog('[update] demo overlay — no force update'); return }
            forceFullUpdate()
          }}
          onClose={() => {
            if (demoUpdateInfo) {
              setDemoUpdateInfo(null)
              setDemoUpdateDownloading(false)
              setDemoUpdateProgress(null)
            } else {
              setUpdateInfo(null)
              setUpdateDownloading(false)
              setUpdateProgress(null)
            }
          }}
        />
      )}
    </div>
  )
}
