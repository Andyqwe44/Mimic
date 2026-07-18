// ═══ Log Panel — real-time log viewer ═══
// Two modes: compact (right sidebar, last 100 lines, no history) and full
// (Log tab: current session + lazy-loaded history file cards).
import { useState, useEffect, useRef } from 'react'
import { FileText, ChevronDown, ArrowDown, Copy, Check, RefreshCw, Pin } from 'lucide-react'
import { Tooltip } from './Toolkit'
import { useTranslation } from 'react-i18next'
import { logMgr, addLog, hostCall } from '../lib/bridge'
import { COLLAPSIBLE_HEADER } from '../lib/constants'
import type { HistoryFile } from '../lib/types'

export function LogPanel({
  compact,
  expanded: exp,
  onToggle,
  keepFiles,
  pinned,
  onTogglePin,
}: {
  compact?: boolean
  expanded?: boolean
  onToggle?: () => void
  keepFiles?: number
  pinned?: boolean
  onTogglePin?: () => void
}) {
  const { t } = useTranslation()

  // ── Panel state ──
  const [localExpanded, setLocalExpanded] = useState(true)
  const expanded = exp !== undefined ? exp : localExpanded
  const toggle = onToggle || (() => setLocalExpanded((v) => !v))
  const scrollRef = useRef<HTMLDivElement>(null)
  const sessionScrollRef = useRef<HTMLDivElement>(null)
  const [scrolledUp, setScrolledUp] = useState(false)
  const [cardsScrolledUp, setCardsScrolledUp] = useState<Set<number>>(new Set())
  const cardScrollRefs = useRef<Map<number, HTMLDivElement>>(new Map())
  const [historyFiles, setHistoryFiles] = useState<HistoryFile[]>([])
  const [openFiles, setOpenFiles] = useState<Set<number>>(new Set())
  const [currentExpanded, setCurrentExpanded] = useState(true)
  const [sessionCopied, setSessionCopied] = useState(false)
  const [copiedFileIdx, setCopiedFileIdx] = useState<number | null>(null)
  // ── Copy feedback state ──
  const [refreshingIdx, setRefreshingIdx] = useState<number | null>(null)
  const [entries, setEntries] = useState(logMgr.getAll())

  // ── Subscribe to LogManager changes ──
  useEffect(() => {
    setEntries(logMgr.getAll())
    return logMgr.subscribe(() => setEntries([...logMgr.getAll()]))
  }, [])

  // ── Load history file list (full mode only, not compact sidebar) ──
  useEffect(() => {
    if (compact) return
    logMgr.loadHistory(keepFiles ?? 5).then(setHistoryFiles)
  }, [keepFiles, compact])

  // ── Format single log line for display ──
  // Normal: [HH:MM:SS.ms] message
  // Collapsed (count > 1): [firstTs → lastTs] message ×N
  const formatLine = (e: { ts: string; msg: string; count?: number; firstTs?: string }) => {
    if (e.count && e.count > 1) {
      const range = e.firstTs && e.firstTs !== e.ts ? `${e.firstTs} → ${e.ts}` : e.ts
      return `[${range}] ${e.msg} ×${e.count}`
    }
    return `[${e.ts}] ${e.msg}`
  }
  const currentLines = entries.map(formatLine)
  const displayLines = compact ? currentLines.slice(-100) : currentLines.slice(-500)

  useEffect(() => {
    const ref = compact ? scrollRef.current : sessionScrollRef.current
    if (!ref) return
    const onScroll = () => {
      const atBottom = ref.scrollTop + ref.clientHeight >= ref.scrollHeight - 40
      setScrolledUp(!atBottom)
    }
    ref.addEventListener('scroll', onScroll, { passive: true })
    return () => ref.removeEventListener('scroll', onScroll)
  }, [compact])

  const entryCount = entries.length
  useEffect(() => {
    const ref = compact ? scrollRef.current : sessionScrollRef.current
    if (!ref || scrolledUp) return
    requestAnimationFrame(() => {
      ref.scrollTop = ref.scrollHeight
    })
  }, [entryCount, compact, scrolledUp])

  // ── Full-card mode (Log tab) ──
  // Shows two card types: "Current Session" (ring buffer) + per-file "History" cards
  if (!compact) {
    return (
      <div className="flex-1 min-h-0 overflow-y-auto p-6 space-y-3">
        {/* Current session card */}
        <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
          <div
            role="button"
            tabIndex={0}
            onClick={() => {
              setCurrentExpanded((v) => !v)
              addLog(`[Log] Current Session ${currentExpanded ? 'collapsed' : 'expanded'}`)
            }}
            onKeyDown={(e) => {
              if (e.key === 'Enter' || e.key === ' ') {
                ;(e.currentTarget as HTMLElement).click()
              }
            }}
            className={COLLAPSIBLE_HEADER}
          >
            <div className="flex items-center gap-2">
              <FileText className="w-4 h-4 text-accent shrink-0" />
              <span className="text-sm font-medium text-text-primary">{t('log.current_session')}</span>
              <span className="text-xs text-text-muted">({displayLines.length})</span>
            </div>
            <div className="flex items-center gap-0.5">
              <Tooltip text={scrolledUp ? t('log.scroll_bottom') : t('log.already_bottom')}>
                <button
                  onClick={(e) => {
                    e.stopPropagation()
                    sessionScrollRef.current?.scrollTo({
                      top: sessionScrollRef.current.scrollHeight,
                      behavior: 'smooth',
                    })
                  }}
                  disabled={!scrolledUp}
                  className={`p-1 rounded-md transition-colors ${scrolledUp ? 'text-accent bg-accent/15 hover:bg-accent/25' : 'text-text-muted/30 cursor-not-allowed'}`}
                >
                  <ArrowDown className="w-3.5 h-3.5" />
                </button>
              </Tooltip>
              <Tooltip text={t('log.copy_all')}>
                <button
                  onClick={(e) => {
                    e.stopPropagation()
                    navigator.clipboard.writeText(displayLines.join('\n'))
                    setSessionCopied(true)
                    setTimeout(() => setSessionCopied(false), 1500)
                  }}
                  className="p-1 rounded-md text-text-secondary hover:text-accent hover:bg-bg-tertiary transition-colors"
                >
                  {sessionCopied ? (
                    <Check className="w-3.5 h-3.5 text-green-400" />
                  ) : (
                    <Copy className="w-3.5 h-3.5" />
                  )}
                </button>
              </Tooltip>
              <ChevronDown
                className={`w-4 h-4 text-text-muted transition-transform duration-150 shrink-0 ${currentExpanded ? 'rotate-180' : ''}`}
              />
            </div>
          </div>
          <div
            className="grid transition-[grid-template-rows] duration-150 ease-out"
            style={{ gridTemplateRows: currentExpanded ? '1fr' : '0fr' }}
          >
            <div className="overflow-hidden min-h-0">
              <div className="border-t border-border" />
              <div
                ref={sessionScrollRef}
                className="h-[400px] overflow-y-auto p-4 font-mono text-xs"
              >
                <div className="min-h-full flex flex-col justify-end space-y-0.5">
                  {displayLines.length === 0 ? (
                    <div className="text-text-muted text-center py-4">{t('log.no_logs_yet')}</div>
                  ) : (
                    displayLines.map((l, i) => {
                      const last = i === displayLines.length - 1
                      const zebra = !last
                        ? i % 2 === 0
                          ? 'bg-white/[0.03]'
                          : 'bg-black/[0.03]'
                        : ''
                      return (
                        <div
                          key={`cur-${i}`}
                          className={`whitespace-pre-wrap break-words ${last ? 'font-semibold text-text-primary' : 'text-text-muted ' + zebra}`}
                          style={{ paddingLeft: '16ch', textIndent: '-16ch' }}
                        >
                          {l}
                        </div>
                      )
                    })
                  )}
                </div>
              </div>
            </div>
          </div>
        </div>

        {/* History file cards */}
        {historyFiles.map((f, fi) => {
          const open = openFiles.has(fi)
          return (
            <div
              key={f.name}
              className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden"
            >
              <div
                role="button"
                tabIndex={0}
                onClick={() => {
                  const s = new Set(openFiles)
                  if (open) {
                    s.delete(fi)
                  } else {
                    s.add(fi)
                    if (f.lines.length === 0) {
                      hostCall('read_log_file', { filename: f.name })
                        .then((res) => {
                          const content = res?.content || ''
                          const newLines = content ? content.split('\n') : ([] as string[])
                          setHistoryFiles((prev) =>
                            prev.map((hf, i) =>
                              i === fi ? { ...hf, lines: newLines } : hf,
                            ),
                          )
                        })
                        .catch(() => {
                          setHistoryFiles((prev) =>
                            prev.map((hf, i) =>
                              i === fi ? { ...hf, lines: [t('log.load_failed')] } : hf,
                            ),
                          )
                        })
                    }
                  }
                  setOpenFiles(s)
                }}
                onKeyDown={(e) => {
                  if (e.key === 'Enter' || e.key === ' ') {
                    ;(e.currentTarget as HTMLElement).click()
                  }
                }}
                className={COLLAPSIBLE_HEADER}
              >
                <div className="flex items-center gap-2 min-w-0">
                  <FileText className="w-4 h-4 text-text-muted shrink-0" />
                  <span className="text-sm font-medium text-text-primary truncate">
                    {f.name}
                  </span>
                  <span className="text-xs text-text-muted shrink-0">
                    {f.lines.length > 0 ? t('log.lines_count', { n: f.lines.length }) : t('log.click_to_load')}
                  </span>
                </div>
                <div className="flex items-center gap-0.5">
                  <Tooltip text={t('log.refresh_file')}>
                    <button
                      onClick={(e) => {
                        e.stopPropagation()
                        setRefreshingIdx(fi)
                        hostCall('read_log_file', { filename: f.name })
                          .then((res) => {
                            const content = res?.content || ''
                            const newLines = content
                              ? content.split('\n')
                              : ([] as string[])
                            setHistoryFiles((prev) =>
                              prev.map((hf, i) =>
                                i === fi ? { ...hf, lines: newLines } : hf,
                              ),
                            )
                          })
                          .catch(() => {})
                          .finally(() => setRefreshingIdx(null))
                      }}
                      className={`p-1 rounded-md text-text-secondary hover:text-accent hover:bg-bg-tertiary transition-colors ${refreshingIdx === fi ? 'animate-spin' : ''}`}
                    >
                      <RefreshCw className="w-3.5 h-3.5" />
                    </button>
                  </Tooltip>
                  <Tooltip text={cardsScrolledUp.has(fi) ? t('log.scroll_bottom') : t('log.already_bottom')}>
                    <button
                      onClick={(e) => {
                        e.stopPropagation()
                        const el = cardScrollRefs.current.get(fi)
                        if (el) el.scrollTo({ top: el.scrollHeight, behavior: 'smooth' })
                      }}
                      disabled={!cardsScrolledUp.has(fi)}
                      className={`p-1 rounded-md transition-colors ${cardsScrolledUp.has(fi) ? 'text-accent bg-accent/15 hover:bg-accent/25' : 'text-text-muted/30 cursor-not-allowed'}`}
                    >
                      <ArrowDown className="w-3.5 h-3.5" />
                    </button>
                  </Tooltip>
                  <Tooltip text={t('log.copy_file_content')}>
                    <button
                      onClick={(e) => {
                        e.stopPropagation()
                        navigator.clipboard.writeText(f.lines.join('\n'))
                        setCopiedFileIdx(fi)
                        setTimeout(() => setCopiedFileIdx(null), 1500)
                      }}
                      className="p-1 rounded-md text-text-secondary hover:text-accent hover:bg-bg-tertiary transition-colors"
                    >
                      {copiedFileIdx === fi ? (
                        <Check className="w-3.5 h-3.5 text-green-400" />
                      ) : (
                        <Copy className="w-3.5 h-3.5" />
                      )}
                    </button>
                  </Tooltip>
                  <ChevronDown
                    className={`w-4 h-4 text-text-muted transition-transform duration-150 shrink-0 ${open ? 'rotate-180' : ''}`}
                  />
                </div>
              </div>
              <div
                className="grid transition-[grid-template-rows] duration-150 ease-out"
                style={{ gridTemplateRows: open ? '1fr' : '0fr' }}
              >
                <div className="overflow-hidden min-h-0">
                  <div className="border-t border-border" />
                  <div
                    ref={(el) => {
                      if (el) cardScrollRefs.current.set(fi, el)
                      else cardScrollRefs.current.delete(fi)
                    }}
                    onScroll={(e) => {
                      const t = e.currentTarget
                      const atBottom =
                        t.scrollTop + t.clientHeight >= t.scrollHeight - 40
                      setCardsScrolledUp((prev) => {
                        const s = new Set(prev)
                        if (!atBottom) s.add(fi)
                        else s.delete(fi)
                        return s
                      })
                    }}
                    className="h-[400px] overflow-y-auto p-4 font-mono text-xs"
                  >
                    <div className="min-h-full flex flex-col justify-end space-y-0.5">
                      {f.lines.length === 0 ? (
                        <div className="text-text-muted text-center py-4">{t('common.loading')}</div>
                      ) : (
                        f.lines.map((l, i) => {
                          const last = i === f.lines.length - 1
                          const zebra = !last
                            ? i % 2 === 0
                              ? 'bg-white/[0.03]'
                              : 'bg-black/[0.03]'
                            : ''
                          return (
                            <div
                              key={i}
                              className={`whitespace-pre-wrap break-words ${last ? 'font-semibold text-text-primary' : 'text-text-muted ' + zebra}`}
                              style={{ paddingLeft: '16ch', textIndent: '-16ch' }}
                            >
                              {l}
                            </div>
                          )
                        })
                      )}
                    </div>
                  </div>
                </div>
              </div>
            </div>
          )
        })}
        {historyFiles.length === 0 && (
          <div className="text-center py-6 text-xs text-text-muted">
            {entries.length === 0 ? t('log.no_logs_yet') : t('log.no_history')}
          </div>
        )}
      </div>
    )
  }

  // ── Compact mode (right sidebar) ──
  // Shows last 100 lines only, no history files. Pin + copy + scroll-down buttons.
  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden flex flex-col min-h-0">
      <div
        role="button"
        tabIndex={0}
        onClick={() => {
          toggle()
          addLog(`[Log] ${!expanded ? 'expanded' : 'collapsed'}`)
        }}
        onKeyDown={(e) => {
          if (e.key === 'Enter' || e.key === ' ') {
            ;(e.currentTarget as HTMLElement).click()
          }
        }}
        className={`${COLLAPSIBLE_HEADER} shrink-0`}
      >
        <div className="flex items-center gap-2">
          <span className="w-5 h-5 rounded bg-amber-400/15 flex items-center justify-center shrink-0">
            <FileText className="w-3 h-3 text-amber-400" />
          </span>
          <span className="text-sm font-medium text-text-primary">{t('log.title')}</span>
          <span className="text-xs text-text-muted">({displayLines.length})</span>
        </div>
        <div className="flex items-center gap-0.5">
          <Tooltip text={scrolledUp ? t('log.scroll_bottom') : t('log.already_bottom')}>
            <button
              onClick={(e) => {
                e.stopPropagation()
                scrollRef.current?.scrollTo({
                  top: scrollRef.current.scrollHeight,
                  behavior: 'smooth',
                })
              }}
              disabled={!scrolledUp}
              className={`p-1 rounded-md transition-colors ${scrolledUp ? 'text-accent bg-accent/15 hover:bg-accent/25' : 'text-text-muted/30 cursor-not-allowed'}`}
            >
              <ArrowDown className="w-3.5 h-3.5" />
            </button>
          </Tooltip>
          <Tooltip text={t('log.copy_log')}>
            <button
              onClick={(e) => {
                e.stopPropagation()
                navigator.clipboard.writeText(displayLines.join('\n'))
                setSessionCopied(true)
                setTimeout(() => setSessionCopied(false), 1500)
              }}
              className="p-1 rounded-md text-text-secondary hover:text-accent hover:bg-bg-tertiary transition-colors"
            >
              {sessionCopied ? (
                <Check className="w-3.5 h-3.5 text-green-400" />
              ) : (
                <Copy className="w-3.5 h-3.5" />
              )}
            </button>
          </Tooltip>
          {pinned !== undefined && onTogglePin && (
            <Tooltip text={pinned ? t('log.unpin_tip') : t('log.pin_tip')}>
              <button
                onClick={(e) => {
                  e.stopPropagation()
                  onTogglePin()
                }}
                className={`p-1 rounded-md transition-colors ${pinned ? 'text-accent hover:bg-bg-tertiary' : 'text-text-secondary hover:text-text-primary hover:bg-bg-tertiary'}`}
              >
                <Pin className={`w-3.5 h-3.5 ${pinned ? 'fill-current' : ''}`} />
              </button>
            </Tooltip>
          )}
          <ChevronDown
            className={`w-4 h-4 text-text-muted transition-transform duration-150 ${expanded ? 'rotate-180' : ''}`}
          />
        </div>
      </div>
      <div
        className="grid transition-[grid-template-rows] duration-150 ease-out flex-1 min-h-0"
        style={{ gridTemplateRows: expanded ? '1fr' : '0fr' }}
      >
        <div className="overflow-hidden min-h-0" data-layout-measure="">
          <div className="border-t border-border" />
          <div ref={scrollRef} className="h-[180px] overflow-y-auto p-4">
            {displayLines.length === 0 ? (
              <div className="flex items-center justify-center h-full text-sm text-text-muted">
                {t('log.no_logs')}
              </div>
            ) : (
              <div className="space-y-1 font-mono text-xs text-text-muted pt-1">
                {displayLines.slice(-100).map((l, i, arr) => {
                  const last = i === arr.length - 1
                  const zebra = !last
                    ? i % 2 === 0
                      ? 'bg-white/[0.03]'
                      : 'bg-black/[0.03]'
                    : ''
                  return (
                    <div
                      key={`c-${i}`}
                      className={
                        last ? 'font-semibold text-text-primary' : 'text-text-muted ' + zebra
                      }
                    >
                      {l}
                    </div>
                  )
                })}
              </div>
            )}
          </div>
        </div>
      </div>
    </div>
  )
}
