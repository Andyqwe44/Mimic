// ═══ WebView2 WebMessage bridge — replaces Tauri invoke() ═══
//   JS hostCall() → postMessage(JSON) → C++ HandleWebMessage → PostWebMessageAsJson
//   → JS 'message' event → resolve by msg.id
import type { LogEntry, HistoryFile } from './types'

// ── Pending call tracker ──
type PendingCall = {
  resolve: (value: any) => void
  reject: (reason: any) => void
  timer: ReturnType<typeof setTimeout>   // 30s timeout
}

let _callId = 0
const _pending = new Map<number, PendingCall>()   // id → {resolve, reject, timer}

// ── hostCall — async C++ command invocation ──
// Auto-unwraps {id, result} envelope → caller receives raw result.
// 30s timeout rejects with descriptive error.
export function hostCall(cmd: string, args?: Record<string, any>): Promise<any> {
  return new Promise((resolve, reject) => {
    const id = ++_callId
    const timer = setTimeout(() => {
      _pending.delete(id)
      reject(new Error(`hostCall timeout: ${cmd}`))
    }, 30000)
    _pending.set(id, {
      resolve: (raw: any) => {
        resolve(raw && typeof raw === 'object' && 'result' in raw ? raw.result : raw)
      },
      reject,
      timer,
    })
    try {
      ;(window as any).chrome.webview.postMessage(JSON.stringify({ cmd, id, args: args || {} }))
    } catch (e) {
      clearTimeout(timer)
      _pending.delete(id)
      reject(e)
    }
  })
}

// ── Smooth theme switch — 200ms CSS transition class ──
export function applyTheme(isDark: boolean) {
  document.documentElement.classList.add('theme-switching')
  document.documentElement.classList.toggle('dark', isDark)
  setTimeout(() => document.documentElement.classList.remove('theme-switching'), 220)
}

// ── Timestamp formatter — HH:MM:SS.ms ──
function timeStr() {
  const d = new Date()
  return `${d.getHours().toString().padStart(2, '0')}:${d.getMinutes().toString().padStart(2, '0')}:${d.getSeconds().toString().padStart(2, '0')}.${d.getMilliseconds().toString().padStart(3, '0')}`
}

// ═══ LogManager — single source of truth for all log views ═══
// Three consumers (right panel, Log tab, disk files) see identical content.
// Entries flow: TS addLog → LogManager.entries → React (0ms)
//            + hostCall('log_ui_event') → C++ LOG() → ring buffer + file
// C++ LOG()  → 'message' event → addRemote → LogManager.entries → React
class LogManager {
  private entries: LogEntry[] = []
  private listeners = new Set<() => void>()
  private initialSyncDone = false

  // ── TS-side add (UI events, user actions) ──
  add(msg: string) {
    this.entries.push({ ts: timeStr(), msg: `[ui] ${msg}` })
    this.listeners.forEach((f) => f())
    hostCall('log_ui_event', { event: msg, detail: '' }).catch(() => {})
  }

  // ── C++-side add (remote log push via 'message' event) ──
  // Dedup by (ts, msg) — prevents double-insert when TS also wrote the entry
  addRemote(ts: string, tag: string, msg: string) {
    const dup = this.entries.find((e) => e.ts === ts && e.msg === `[${tag}] ${msg}`)
    if (dup) return
    this.entries.push({ ts, msg: `[${tag}] ${msg}` })
    if (this.entries.length > 500) this.entries = this.entries.slice(-500)
    this.listeners.forEach((f) => f())
  }

  getAll(): LogEntry[] {
    return this.entries
  }

  // ── Subscribe to entry changes (React setState in components) ──
  subscribe(fn: () => void): () => void {
    this.listeners.add(fn)
    return () => {
      this.listeners.delete(fn)
    }
  }

  // ── One-time ring buffer sync at startup (before WebView2 was ready) ──
  async initSync() {
    if (this.initialSyncDone) return
    this.initialSyncDone = true
    try {
      const res = await hostCall('read_live_log')
      const raw = typeof res === 'string' ? res : res?.lines || ''
      if (!raw) return
      const lines = raw.split('\n').filter((l: string) => l.trim())
      for (const line of lines) {
        const m = line.match(/^\[(\d{2}:\d{2}:\d{2}\.\d{3})\]\s\[(\w+)\]\s(.+)$/)
        if (m) {
          this.entries.push({ ts: m[1], msg: `[${m[2]}] ${m[3]}` })
        }
      }
      this.entries.sort((a, b) => a.ts.localeCompare(b.ts))
      if (this.entries.length > 500) this.entries = this.entries.slice(-500)
      this.listeners.forEach((f) => f())
    } catch (_) {}
  }

  clear() {
    this.entries = []
    this.listeners.forEach((f) => f())
    this.initialSyncDone = false
    hostCall('clear_log').catch(() => {})
    setTimeout(() => this.initSync(), 100)
  }

  // ── Load history file list (Log tab only, not compact mode) ──
  async loadHistory(maxFiles: number): Promise<HistoryFile[]> {
    try {
      const data = await hostCall('read_logs', { max_files: maxFiles })
      const payload = data?.result || data
      const files = payload?.files || []
      return files.map((f: any) => ({ name: f.name, lines: [] as string[] }))
    } catch (e) {
      return []
    }
  }
}

// ── Singleton ──
export const logMgr = new LogManager()

// ── Convenience: add UI-side log entry ──
export function addLog(msg: string) {
  logMgr.add(msg)
}

// ── C++ → JS message listener (responses + remote log push) ──
if (typeof (window as any).chrome?.webview !== 'undefined') {
  ;(window as any).chrome.webview.addEventListener('message', (e: any) => {
    try {
      const msg = typeof e.data === 'string' ? JSON.parse(e.data) : e.data
      // Log push: C++ capture_log_write_msg → notify callback → PostWebMessage
      if (msg.type === 'log') {
        logMgr.addRemote(msg.ts, msg.tag, msg.msg)
        return
      }
      // Command response: {id, result} envelope → resolve matching pending call
      const pending = _pending.get(msg.id)
      if (pending) {
        clearTimeout(pending.timer)
        _pending.delete(msg.id)
        pending.resolve(msg.result)
      }
    } catch {}
  })
}
