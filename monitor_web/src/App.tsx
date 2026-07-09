// ═══ App — Game Agent Monitor ═══
import { useState, useEffect, useRef, useCallback } from 'react'
import { TopBar } from './components/TopBar'
import { BottomBar } from './components/BottomBar'
import { Tooltip } from './components/Toolkit'
import { ConnectionPanel } from './components/ConnectionPanel'
import { ScreenshotPanel } from './components/ScreenshotPanel'
import { LogPanel } from './components/LogPanel'
import { SettingsView } from './components/SettingsView'
import { MonitorView } from './components/MonitorView'
import { hostCall, logMgr, addLog, applyTheme } from './lib/bridge'
import { cantCaptureMinimized } from './lib/constants'
import type { WindowInfo } from './lib/types'

// ── Layout constants ──
const MIN_LEFT_WIDTH = 360
const DEFAULT_RIGHT_WIDTH = 324

export default function App() {
  const [tab, setTab] = useState<'Monitor' | 'Log' | 'Settings'>('Settings')
  const [running, setRunning] = useState(false)
  const [appVersion, setAppVersion] = useState('v0.3.0')
  const [rightWidth, setRightWidth] = useState(DEFAULT_RIGHT_WIDTH)
  const [rightCollapsed, setRightCollapsed] = useState(false)
  const [connectionExpanded, setConnectionExpanded] = useState(true)
  const [screenshotExpanded, setScreenshotExpanded] = useState(false)
  const [logExpanded, setLogExpanded] = useState(true)
  const connectionExpandedRef = useRef(connectionExpanded)
  connectionExpandedRef.current = connectionExpanded
  const screenshotExpandedRef = useRef(screenshotExpanded)
  screenshotExpandedRef.current = screenshotExpanded
  const logExpandedRef = useRef(logExpanded)
  logExpandedRef.current = logExpanded

  // ── Pin lock ──
  const connPinLocked = useRef<boolean | null>(null)
  const ssPinLocked = useRef<boolean | null>(null)
  const logPinLocked = useRef<boolean | null>(null)
  const [connectionPinned, setConnectionPinned] = useState(false)
  const [screenshotPinned, setScreenshotPinned] = useState(false)
  const [logPinned, setLogPinned] = useState(false)
  const ssHasContentRef = useRef(false)

  // Safe setters
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

  // Pin toggles
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

  // ── Theme ──
  const [theme, setTheme] = useState<'light' | 'dark' | 'system'>('light')
  const [systemDark, setSystemDark] = useState(false)
  const resolvedDark = theme === 'system' ? systemDark : theme === 'dark'

  useEffect(() => {
    const mq = window.matchMedia('(prefers-color-scheme: dark)')
    setSystemDark(mq.matches)
    const onChange = (e: MediaQueryListEvent) => setSystemDark(e.matches)
    mq.addEventListener('change', onChange)
    return () => mq.removeEventListener('change', onChange)
  }, [])

  useEffect(() => {
    applyTheme(resolvedDark)
  }, [resolvedDark])

  // ── Right panel auto-layout ──
  const H = useRef({ C: 180, S: 300, L: 250, Cp: 44, Sp: 44, Lp: 44 })
  const GAP = 60
  const prevClientH = useRef(0)
  const guard = useRef({ C: 0, S: 0, L: 0 })

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

  useEffect(() => {
    logMgr.initSync()
  }, [])

  // Initial layout check
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

  // Resize auto-layout
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

  // Drag overflow check
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

  // Horizontal auto-collapse
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

  // ── Capture state ──
  const isResizing = useRef(false)
  const [selWindow, setSelWindow] = useState<WindowInfo>({
    title: ' Entire Desktop',
    category: 'desktop',
    hwnd: 0,
  })
  const [screenRatio, setScreenRatio] = useState(16 / 9)
  const [snapMethod, setSnapMethod] = useState('dxgi')
  const [streamMethod, setStreamMethod] = useState('wgc')
  const [autoSnap, setAutoSnap] = useState(true)
  const [autoStream, setAutoStream] = useState(true)
  const [renderMethod, setRenderMethod] = useState('shared')
  const [expectedCaptureState, setExpectedCaptureState] = useState('desktop')
  const [keepFiles, setKeepFiles] = useState(5)
  const [inputMethod, setInputMethod] = useState('sendinput')
  const [devMode, setDevMode] = useState(false)
  const [saveCaptureFrames, setSaveCaptureFrames] = useState(false)
  const [saveStreamFrames, setSaveStreamFrames] = useState(false)
  const [frameDumpDir, setFrameDumpDir] = useState('')
  const [winState, setWinState] = useState('desktop')
  const lastWinStateRef = useRef('desktop')

  // Capture operation state machine
  const opStateRef = useRef<'idle' | 'snapshotting' | 'streaming'>('idle')
  const snapCancelRef = useRef(0)
  const [previewing, setPreviewing] = useState(false)
  const previewingRef = useRef(false)
  const snapshotRef = useRef(false)
  const snapshotStartRef = useRef(0)
  const [capMethod, setCapMethod] = useState('')
  const [snapshotLatency, setSnapshotLatency] = useState<number | null>(null)
  const [streamFps, setStreamFps] = useState(0)
  const [targetDims, setTargetDims] = useState<{w:number,h:number} | null>(null)
  const [agentConnected] = useState(false) // placeholder — future TCP agent detection

  useEffect(() => {
    ;(async () => {
      try {
        const si = await hostCall('screen_info')
        setScreenRatio(si.w / si.h)
      } catch (_) {}
    })()
  }, [])

  // Window state polling
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

  // Auto-select capture methods
  useEffect(() => {
    const isDesktop = selWindow.hwnd === 0
    if (autoSnap) {
      setSnapMethod(isDesktop || winState === 'minimized' ? 'dxgi' : 'wgc')
    }
    if (autoStream) {
      setStreamMethod('wgc')
    }
  }, [selWindow.hwnd, winState, autoSnap, autoStream])

  // ── Capture operations ──
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

  const togglePreview = useCallback(async () => {
    if (previewing) {
      await stopStream()
    } else {
      await startStream()
    }
  }, [previewing, stopStream, startStream])

  useEffect(() => {
    return () => {
      previewingRef.current = false
      snapshotRef.current = false
      hostCall('capture_stream_stop').catch(() => {})
    }
  }, [])

  // ── Resize handler ──
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

  // ── Render ──
  return (
    <div className="h-full flex flex-col bg-bg-primary">
      <TopBar
        tab={tab}
        setTab={setTab}
        running={running}
        onStart={() => setRunning(true)}
        onStop={() => setRunning(false)}
        dark={resolvedDark}
        onToggleTheme={() => setTheme(resolvedDark ? 'light' : 'dark')}
      />
      <div className="flex-1 flex overflow-hidden">
        <div
          className="flex-1 flex flex-col overflow-hidden border-r border-border"
          style={{ minWidth: MIN_LEFT_WIDTH }}
        >
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
              inputMethod={inputMethod} setInputMethod={setInputMethod}
            />
          )}
          {tab === 'Monitor' && (
            <MonitorView
              selWin={selWindow} winState={winState}
              capMethod={capMethod}
              snapMethod={snapMethod} streamMethod={streamMethod}
              previewing={previewing}
              snapshotLatency={snapshotLatency}
              onTakeSnapshot={takeSnapshot}
              onTogglePreview={togglePreview}
              inputMethod={inputMethod}
              targetDims={targetDims}
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
          {tab === 'Log' && <LogPanel keepFiles={keepFiles} />}
          <BottomBar
            selWin={selWindow.title}
            snapMethod={snapMethod} streamMethod={streamMethod}
            previewing={previewing} fps={streamFps}
            targetDims={targetDims}
            appVersion={appVersion}
            agentConnected={agentConnected}
          />
        </div>
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
        {!rightCollapsed && (
          <div
            ref={rightPanelRef}
            className="flex flex-col p-3 gap-3 overflow-hidden min-h-0"
            style={{ width: rightWidth, minWidth: 324, maxWidth: 400 }}
          >
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
            <div className="shrink-0">
              <LogPanel
                compact expanded={logExpanded}
                onToggle={() => setLogPanelExpanded(!logExpandedRef.current)}
                pinned={logPinned} onTogglePin={toggleLogPin}
              />
            </div>
            <div className="flex-1" />
          </div>
        )}
      </div>
    </div>
  )
}
