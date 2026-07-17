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

// Rectangle in screen coordinates
export interface Rect { x: number; y: number; w: number; h: number }

// Single log entry — timestamp (HH:MM:SS.ms) + message.
// When count > 1, this entry represents collapsed consecutive duplicates:
// firstTs = timestamp of first occurrence, ts = timestamp of last occurrence.
export type LogEntry = {
  ts: string
  msg: string
  count?: number    // >1 when collapsed — how many consecutive identical entries
  firstTs?: string  // timestamp of the first entry in the collapsed range
}
