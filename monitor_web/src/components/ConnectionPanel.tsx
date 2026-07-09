// ═══ Connection Panel (MXU-style collapsible card) ───
import { useState, useEffect } from 'react'
import { ChevronDown, MonitorUp, Unlink, Pin } from 'lucide-react'
import { Tooltip, ActionBtn } from './Toolkit'
import { TargetPickerModal } from './TargetPickerModal'
import { addLog } from '../lib/bridge'
import {
  COLLAPSIBLE_HEADER,
  METHOD_SHORT,
  STATE_LABEL,
  cantCaptureMinimized,
} from '../lib/constants'
import type { WindowInfo } from '../lib/types'

export function ConnectionPanel({
  onSelect,
  onDisconnect,
  snapMethod: _snapMethod,
  setSnapMethod,
  streamMethod,
  setStreamMethod,
  selWin,
  winState,
  expectedCaptureState: _expectedCaptureState,
  setExpectedCaptureState,
  expanded,
  onToggle,
  pinned,
  onTogglePin,
}: {
  onSelect: (w: WindowInfo) => void
  onDisconnect?: () => void
  snapMethod: string
  setSnapMethod?: (m: string) => void
  streamMethod: string
  setStreamMethod?: (m: string) => void
  selWin?: WindowInfo
  winState: string
  expectedCaptureState?: string
  setExpectedCaptureState?: (s: string) => void
  expanded: boolean
  onToggle: () => void
  pinned?: boolean
  onTogglePin?: () => void
}) {
  const [pickerOpen, setPickerOpen] = useState(false)
  const [selTitle, setSelTitle] = useState(' Entire Desktop')
  const [ip, setIp] = useState('127.0.0.1')
  const [port, setPort] = useState('9999')
  const isDesktop = selTitle === ' Entire Desktop'

  const handleSelectWindow = (w: WindowInfo) => {
    setSelTitle(w.title)
    onSelect(w)
  }

  const handleSelectMode = (method: string, expectedState: string) => {
    if (setSnapMethod) setSnapMethod(method)
    if (setStreamMethod) setStreamMethod(method)
    if (setExpectedCaptureState) setExpectedCaptureState(expectedState)
  }

  useEffect(() => {
    if (selWin && selWin.title !== selTitle) {
      setSelTitle(selWin.title)
    }
  }, [selWin?.title])

  const cantCapture = !isDesktop && cantCaptureMinimized(streamMethod, winState)
  const recommendedMethod = winState === 'minimized' ? 'dxgi' : 'wgc'

  return (
    <div className="bg-bg-secondary rounded-xl ring-1 ring-inset ring-border overflow-hidden">
      <div
        role="button"
        tabIndex={0}
        onClick={() => {
          onToggle()
          addLog(`[Connection] ${!expanded ? 'expanded' : 'collapsed'}`)
        }}
        onKeyDown={(e) => {
          if (e.key === 'Enter' || e.key === ' ') {
            ;(e.currentTarget as HTMLElement).click()
          }
        }}
        className={COLLAPSIBLE_HEADER}
      >
        <div className="flex items-center gap-2 min-w-0">
          <span className="w-5 h-5 rounded bg-blue-400/15 flex items-center justify-center shrink-0">
            <MonitorUp className="w-3 h-3 text-blue-400" />
          </span>
          <span className="text-sm font-medium text-text-primary shrink-0">Connection</span>
          <span className="text-[11px] font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded shrink-0">
            {STATE_LABEL[winState] || winState}
          </span>
          <span className="text-[11px] font-medium text-accent bg-accent/10 px-1.5 py-0.5 rounded shrink-0">
            {METHOD_SHORT[recommendedMethod] || recommendedMethod}
          </span>
        </div>
        <div className="flex items-center gap-2 ml-2">
          {cantCapture && <span className="text-xs text-error shrink-0">⚠</span>}
          {onTogglePin && (
            <Tooltip text={pinned ? '取消固定' : '固定面板'}>
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
        className="grid transition-[grid-template-rows] duration-150 ease-out"
        style={{ gridTemplateRows: expanded ? '1fr' : '0fr' }}
      >
        <div className="overflow-hidden min-h-0" data-layout-measure="">
          <div className="border-t border-border" />
          <div className="max-h-[360px] overflow-y-auto p-3 space-y-2">
            {cantCapture && (
              <div className="text-xs text-error bg-red-500/10 rounded-lg px-2 py-1.5">
                窗口已最小化，{streamMethod.toUpperCase()}{' '}
                无法截取。请切换为 WGC 或将窗口恢复前台。
              </div>
            )}
            <div className="flex justify-between">
              <div className="flex items-center gap-1.5">
                <Tooltip text="已选择的目标窗口（只读，请用Select选择）">
                  <input
                    value={selTitle}
                    readOnly
                    placeholder="Window Title"
                    className="w-36 h-8 rounded-lg border border-border bg-bg-primary px-2 text-xs outline-none cursor-default text-text-muted truncate"
                  />
                </Tooltip>
                {onDisconnect && (
                  <Tooltip text="断开当前窗口连接，回到桌面">
                    <button
                      onClick={() => {
                        onDisconnect()
                        setSelTitle(' Entire Desktop')
                      }}
                      className="h-8 w-8 flex items-center justify-center rounded-md bg-amber-500/10 hover:bg-amber-500/20 text-amber-400 border border-amber-500/20 transition-colors shrink-0"
                    >
                      <Unlink className="w-3.5 h-3.5" />
                    </button>
                  </Tooltip>
                )}
              </div>
              <ActionBtn
                icon={<MonitorUp className="w-3.5 h-3.5" />}
                label="Select"
                title="选择要捕获的窗口或桌面"
                variant="primary"
                onClick={() => {
                  setPickerOpen(true)
                  addLog('[Window] opening picker')
                }}
                className="h-8"
              />
            </div>
            <div className="flex justify-between">
              <Tooltip text="AI模型服务器IP地址">
                <input
                  value={ip}
                  onChange={(e) => {
                    const v = e.target.value
                    if (v.includes('::')) {
                      const [a, b] = v.split('::', 2)
                      setIp(a.trim())
                      if (b?.trim()) setPort(b.trim())
                    } else setIp(v)
                  }}
                  placeholder="IP Address"
                  className="w-[184px] h-8 rounded-lg border border-border bg-bg-primary px-2 text-xs text-text-primary outline-none focus:border-accent transition-colors placeholder:text-text-muted"
                />
              </Tooltip>
              <Tooltip text="Port端口号">
                <input
                  value={port}
                  onChange={(e) => {
                    const v = e.target.value
                    if (v.includes('::')) {
                      const [a, b] = v.split('::', 2)
                      setPort(a.trim())
                      if (b?.trim()) setIp(b.trim())
                    } else setPort(v)
                  }}
                  placeholder="Port"
                  className="w-20 h-8 rounded-lg border border-border bg-bg-primary px-2 text-xs text-text-primary outline-none focus:border-accent transition-colors placeholder:text-text-muted"
                />
              </Tooltip>
            </div>
          </div>
        </div>
      </div>
      <TargetPickerModal
        open={pickerOpen}
        onClose={() => setPickerOpen(false)}
        onSelectWindow={handleSelectWindow}
        onSelectMode={handleSelectMode}
      />
    </div>
  )
}
