// ═══ App — Game Agent Monitor root ═══
// Orchestrates: tab routing, right-panel layout, capture state machine,
// theme, pin lock, window polling, and resize handling.
import { useState, useEffect, useRef, useCallback } from 'react'
import { TopBar } from './components/TopBar'
import { BottomBar } from './components/BottomBar'
import { Tooltip } from './components/Toolkit'
import { ConnectionPanel } from './components/ConnectionPanel'
import { ScreenshotPanel } from './components/ScreenshotPanel'
import { LogPanel } from './components/LogPanel'
import { SettingsView } from './components/SettingsView'
import { MonitorView, type MonitorApi } from './components/MonitorView'
import { SelfTestModal } from './components/SelfTestModal'
import { UpdateModal, type UpdateInfo } from './components/UpdateModal'
import { LoadingScreen } from './components/LoadingScreen'
import { hostCall, logMgr, addLog, applyTheme, onUpdateProgress, type UpdateProgressMsg } from './lib/bridge'
import { runSelfTest, sleep, type SelfTestState } from './lib/selftest'
import { cantCaptureMinimized } from './lib/constants'
import type { WindowInfo, Rect } from './lib/types'

// ── Layout constants ──
const MIN_LEFT_WIDTH = 360
const DEFAULT_RIGHT_WIDTH = 324

// DELIBERATE test delay so the startup skeleton screen stays visible long enough
// to observe during development. Real startup needs NO artificial wait — the
// skeleton would otherwise flash by. Set to 0 (or delete the timer) for production.
const SPLASH_TEST_MS = 0

export default function App() {
  // ═══ UI state ═══
  const [tab, setTab] = useState<'Monitor' | 'Log' | 'Settings'>('Settings')
  const [running, setRunning] = useState(false)
  // Initialised from the compile-time version (version.h via Vite) so the splash
  // shows it instantly; get_version overwrites it at runtime (they match).
  const [appVersion, setAppVersion] = useState(`v${__APP_VERSION__}`)
  const [appReady, setAppReady] = useState(false)   // false = show startup splash overlay
  const [previewSkeleton, setPreviewSkeleton] = useState(false)  // Dev: manual skeleton preview
  const [updateInfo, setUpdateInfo] = useState<UpdateInfo | null>(null)
  const [updateDownloading, setUpdateDownloading] = useState(false)
  const [updateProgress, setUpdateProgress] = useState<UpdateProgressMsg | null>(null)
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
  const [theme, setTheme] = useState<'light' | 'dark' | 'system'>('light')
  const [systemDark, setSystemDark] = useState(false)
  const resolvedDark = theme === 'system' ? systemDark : theme === 'dark'

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

  // ── Update check / download handlers ──
  const checkForUpdate = useCallback(async () => {
    addLog('[update] checking...')
    try {
      const info = await hostCall('check_update')
      if (info?.has_update) {
        setUpdateInfo({
          current: info.current,
          latest: info.latest,
          name: info.name || '',
          body: info.body || '',
          url: '', // Phase 2: multi-file diff replaces single URL
          diff: info.diff || [],
          message: info.message || '',
          mandatory: !!info.mandatory,
          mode: info.mode || 'incremental',
        } as any)
        const fileCount = info.diff?.length || 0
        addLog(`[update] v${info.latest} available (${fileCount} files, ${info.mode || 'incremental'})`)
      } else if (info?.needs_full_installer) {
        // Manifest schema newer than this client understands — the bootstrap
        // can't safely incrementally update. Point the user at the full package.
        addLog(`[update] full reinstall required — download: ${info.download_url || 'Gitee releases'}`)
      } else {
        addLog(info?.ok === false
          ? `[update] check failed: ${info?.error || 'unknown'}`
          : `[update] already latest (v${info?.current || '?'})`)
      }
    } catch (e: any) {
      addLog(`[update] check error: ${e?.message || e}`)
    }
  }, [])

  // Kick off a download for the given diff. C++ returns immediately and drives
  // the UI via update_progress pushes; the app exits when the updater launches.
  const startDownload = useCallback(async (diff: any[]) => {
    if (!diff?.length) { addLog('[update] no files to download'); return }
    setUpdateProgress(null)
    setUpdateDownloading(true)
    addLog(`[update] downloading ${diff.length} files...`)
    try {
      const res = await hostCall('download_update', { diff: JSON.stringify(diff) })
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
    startDownload((updateInfo as any).diff)
  }, [updateInfo, startDownload])

  // Full update: re-query with force_full to get the complete file set, then download.
  const forceFullUpdate = useCallback(async () => {
    addLog('[update] fetching full package...')
    try {
      const info = await hostCall('check_update', { force_full: true })
      if (info?.has_update) {
        setUpdateInfo({
          current: info.current, latest: info.latest,
          name: info.name || '', body: info.body || '', url: '',
          diff: info.diff || [],
          message: info.message || '', mandatory: !!info.mandatory, mode: info.mode || 'full',
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

  // Load saved settings on startup
  useEffect(() => {
    hostCall('get_settings').then((res: any) => {
      const s = res?.settings
      if (!s || typeof s !== 'object') return
      if (s.theme) setTheme(s.theme)
      if (s.mouseMode) setMouseMode(s.mouseMode)
      if (s.keyMode) setKeyMode(s.keyMode)
      if (s.mappingHotkey) setMappingHotkey(s.mappingHotkey)
      if (typeof s.devMode === 'boolean') setDevMode(s.devMode)
      if (s.selfTargetMode) setSelfTargetMode(s.selfTargetMode)
      if (typeof s.keepFiles === 'number') setKeepFiles(s.keepFiles)
      if (typeof s.autoSnap === 'boolean') setAutoSnap(s.autoSnap)
      if (typeof s.autoStream === 'boolean') setAutoStream(s.autoStream)
      if (s.snapMethod) setSnapMethod(s.snapMethod)
      if (s.streamMethod) setStreamMethod(s.streamMethod)
      if (s.renderMethod) setRenderMethod(s.renderMethod)
      addLog('[settings] loaded')
    }).catch(() => {})
  }, [])

  // Auto-save settings on change (debounced) — declared early, effect wired after all states below
  const saveTimeout = useRef<ReturnType<typeof setTimeout> | null>(null)
  const saveSetting = useCallback((key: string, value: any) => {
    hostCall('set_setting', { key, value: typeof value === 'boolean' ? `"${value}"` : value }).catch(() => {})
  }, [])

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
    title: ' Entire Desktop',
    category: 'desktop',
    hwnd: 0,
  })
  const [screenRatio, setScreenRatio] = useState(16 / 9)

  // ── Capture method config (snapshot + stream independent) ──
  const [snapMethod, setSnapMethod] = useState('dxgi')
  const [streamMethod, setStreamMethod] = useState('wgc')
  const [autoSnap, setAutoSnap] = useState(true)
  const [autoStream, setAutoStream] = useState(true)
  const [renderMethod, setRenderMethod] = useState('shared')
  const [expectedCaptureState, setExpectedCaptureState] = useState('desktop')

  // ── General settings ──
  const [keepFiles, setKeepFiles] = useState(5)
  const [mouseMode, setMouseMode] = useState<'seize' | 'semi' | 'background'>('background')
  const [keyMode, setKeyMode] = useState<'seize' | 'postmsg' | 'sendmsg'>('postmsg')
  const [mappingEnabled, setMappingEnabled] = useState(false)
  const [mappingHotkey, setMappingHotkey] = useState('F10')

  // ── Dev mode + frame dump ──
  const [devMode, setDevMode] = useState(false)
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
  const [agentConnected] = useState(false) // placeholder — future TCP agent detection

  // ── Self-target detection: GAM window rect + virtual screen rect ──
  const [selfRect, setSelfRect] = useState<Rect | null>(null)
  const [screenRect, setScreenRect] = useState<Rect | null>(null)
  const [selfTargetMode, setSelfTargetMode] = useState<'warn' | 'exclude'>('warn')

  // ── Auto-save settings on change (debounced 1s, placed after all state declarations) ──
  useEffect(() => {
    if (saveTimeout.current) clearTimeout(saveTimeout.current)
    saveTimeout.current = setTimeout(() => {
      saveSetting('theme', theme)
      saveSetting('mouseMode', mouseMode)
      saveSetting('keyMode', keyMode)
      saveSetting('mappingHotkey', mappingHotkey)
      saveSetting('devMode', devMode)
      saveSetting('selfTargetMode', selfTargetMode)
      saveSetting('keepFiles', keepFiles)
      saveSetting('autoSnap', autoSnap)
      saveSetting('autoStream', autoStream)
      saveSetting('snapMethod', snapMethod)
      saveSetting('streamMethod', streamMethod)
      saveSetting('renderMethod', renderMethod)
    }, 1000)
    return () => { if (saveTimeout.current) clearTimeout(saveTimeout.current) }
  }, [theme, mouseMode, keyMode, mappingHotkey, devMode, selfTargetMode,
      keepFiles, autoSnap, autoStream, snapMethod, streamMethod, renderMethod])

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

  const runSelfTestFlow = useCallback(
    async (perCell: number) => {
      if (selfTest.phase === 'running') return
      selfTestAbort.current = false
      setSelfTest({ phase: 'running', done: 0, total: 0 })
      addLog(`[SelfTest] start (perCell=${perCell})`)
      try {
        // 1 — ensure the test_target window is running (never toggle-close it)
        let hwnd = (await hostCall('find_test_target'))?.hwnd || 0
        if (!hwnd) {
          await hostCall('launch_test_target')
          for (let i = 0; i < 40 && !hwnd; i++) {
            await sleep(100)
            hwnd = (await hostCall('find_test_target'))?.hwnd || 0
          }
        }
        if (!hwnd) throw new Error('test_target 窗口未找到')

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

        // 4 — dense sweep (reuses sendMappedClick via the imperative api)
        const summary = await runSelfTest({
          perCell,
          sendClick: (rx, ry, b) => monitorApiRef.current!.sendClick(rx, ry, b),
          onProgress: (done, total) =>
            setSelfTest((s) => (s.phase === 'running' ? { ...s, done, total } : s)),
          shouldAbort: () => selfTestAbort.current,
        })
        setSelfTest({ phase: 'done', summary })
        addLog(
          `[SelfTest] done recv=${summary.received}/${summary.total} cell=${Math.round((summary.cellMatch / summary.total) * 100)}% off=(${summary.meanDx.toFixed(1)},${summary.meanDy.toFixed(1)})`,
        )
      } catch (e: any) {
        setSelfTest({ phase: 'error', error: e?.message || String(e) })
        addLog(`[SelfTest] error: ${e?.message || e}`)
      }
    },
    [selfTest.phase, stopStream, setMappingEnabled],
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
                  title: ' Entire Desktop',
                  category: 'desktop',
                  hwnd: 0,
                })
                setExpectedCaptureState('desktop')
                addLog('[Connection] disconnected, back to desktop')
              }}
              keepFiles={keepFiles} setKeepFiles={setKeepFiles}
              appVersion={appVersion} theme={theme} setTheme={setTheme}
              devMode={devMode} setDevMode={setDevMode}
              saveCaptureFrames={saveCaptureFrames}
              setSaveCaptureFrames={setSaveCaptureFrames}
              saveStreamFrames={saveStreamFrames}
              setSaveStreamFrames={setSaveStreamFrames}
              frameDumpDir={frameDumpDir} setFrameDumpDir={setFrameDumpDir}
              mouseMode={mouseMode} setMouseMode={setMouseMode}
              keyMode={keyMode} setKeyMode={setKeyMode}
              mappingHotkey={mappingHotkey} setMappingHotkey={setMappingHotkey}
              selfTargetMode={selfTargetMode} setSelfTargetMode={setSelfTargetMode}
              onRunSelfTest={runSelfTestFlow}
              selfTestRunning={selfTest.phase === 'running'}
              onCheckUpdate={checkForUpdate}
              hasUpdate={!!updateInfo}
              onPreviewSkeleton={previewSkeletonScreen}
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
            selWin={selWindow.title}
            snapMethod={snapMethod} streamMethod={streamMethod}
            previewing={previewing} fps={streamFps}
            targetDims={targetDims}
            appVersion={appVersion}
            agentConnected={agentConnected}
            hasUpdate={!!updateInfo}
            onCheckUpdate={checkForUpdate}
          />
        </div>
        {/* ── Resize divider ── */}
        <Tooltip
          text={
            rightCollapsed ? '向右拖拽展开面板' : '拖拽调整面板宽度，向右拖到底可折叠'
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
                    title: ' Entire Desktop',
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
        state={selfTest}
        onClose={() => setSelfTest({ phase: 'idle' })}
        onAbort={() => { selfTestAbort.current = true }}
      />

      {/* ── Update available modal ── */}
      {updateInfo && (
        <UpdateModal
          info={updateInfo}
          downloading={updateDownloading}
          progress={updateProgress}
          onDownload={downloadUpdate}
          onForceUpdate={forceFullUpdate}
          onClose={() => { setUpdateInfo(null); setUpdateDownloading(false); setUpdateProgress(null) }}
        />
      )}
    </div>
  )
}
