// ═══ Shared types — used across all components ═══

// Window/desktop entry from C++ list_windows command
export interface WindowInfo {
  title: string
  category: string       // 'desktop' | 'window' | 'process'
  hwnd: number           // window handle, or 0 for desktop, or pid for process
  desktop?: number       // virtual desktop index (D1/D2/...), from registry
}

// Log history file metadata (content loaded on demand)
export interface HistoryFile {
  name: string
  lines: string[]        // empty until user expands the tile
}

// Single log entry — timestamp (HH:MM:SS.ms) + message
export type LogEntry = { ts: string; msg: string }
