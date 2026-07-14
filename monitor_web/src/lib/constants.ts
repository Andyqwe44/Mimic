// ═══ Shared constants ═══
// Note: `rec` / `desc` / mouse·keyboard `name` are i18n keys — use t(...).
// Capture/render `name`+`eng` and keyboard short tags are proper nouns / API abbreviations.

// Collapsible card header (used in 6 places)
export const COLLAPSIBLE_HEADER =
  'w-full flex items-center justify-between px-3 py-2 hover:bg-bg-hover cursor-pointer transition-colors outline-none'

// Selectable option button (capture method + transport)
export const SELECTABLE_BTN =
  'flex items-center w-full px-3 py-2 rounded-lg border transition'

// Methods that cannot capture minimized windows
export const METHOD_SHORT: Record<string, string> = {
  wgc: 'WGC', gdi: 'GDI', dxgi: 'DXGI', printwindow: 'PW',
  screenbitblt: 'SBlt', GDI: 'GDI', 'GDI(GetWindowDC)': 'GDI',
  PrintWindow: 'PW', 'PrintWindow(minimized)': 'PW',
  ScreenBitBlt: 'SBlt', DesktopBlt: 'DXGI', WGC: 'WGC',
}

export const METHODS_NO_MINIMIZED = ['wgc', 'gdi', 'printwindow', 'screenbitblt']

export const cantCaptureMinimized = (method: string, ws: string) =>
  ws === 'minimized' && METHODS_NO_MINIMIZED.includes(method)

// Values are i18n keys — use t(STATE_LABEL[state]) to render
export const STATE_LABEL: Record<string, string> = {
  desktop: 'state.desktop', foreground: 'state.foreground', background: 'state.background',
  minimized: 'state.minimized', hidden: 'state.hidden', closed: 'state.closed', unknown: 'state.unknown',
}

export const STATE_COLOR: Record<string, string> = {
  desktop: 'text-text-muted', foreground: 'text-success',
  background: 'text-accent', minimized: 'text-error',
  hidden: 'text-error', closed: 'text-error', unknown: 'text-text-muted',
}

export const CAPTURE_METHODS = [
  { v: 'wgc',  name: 'WGC', eng: 'GPU FramePool', rec: 'capture.wgc.rec', desc: 'capture.wgc.desc' },
  { v: 'dxgi', name: 'DXGI', eng: 'DesktopBlt',   rec: 'capture.dxgi.rec', desc: 'capture.dxgi.desc' },
]

export const RENDER_METHODS = [
  { v: 'shared', name: 'SharedBuffer', eng: 'Zero-copy COM', rec: 'capture.render_shared.rec', desc: 'capture.render_shared.desc' },
  { v: 'h264',   name: 'H.264',        eng: 'GPU MFT + MSE', rec: 'capture.render_h264.rec', desc: 'capture.render_h264.desc' },
  { v: 'h265',   name: 'H.265',        eng: 'GPU MFT + MSE', rec: 'capture.render_h265.rec', desc: 'capture.render_h265.desc' },
]

export const CAPTURE_MODES = [
  { v: 'foreground', label: 'capture.modes.foreground.label', desc: 'capture.modes.foreground.desc', method: 'wgc' },
  { v: 'background', label: 'capture.modes.background.label', desc: 'capture.modes.background.desc', method: 'wgc' },
  { v: 'minimized',  label: 'capture.modes.minimized.label',  desc: 'capture.modes.minimized.desc',  method: 'dxgi' },
]

export const MOUSE_MODES = [
  { v: 'background' as const, name: 'mouse.background.name', eng: 'PostMessage', rec: 'mouse.background.rec',
    desc: 'mouse.background.desc' },
  { v: 'semi' as const, name: 'mouse.semi.name', eng: 'SendMsg-Cursor', rec: 'mouse.semi.rec',
    desc: 'mouse.semi.desc' },
  { v: 'seize' as const, name: 'mouse.seize.name', eng: 'SendInput', rec: 'mouse.seize.rec',
    desc: 'mouse.seize.desc' },
]

export const KEYBOARD_MODES = [
  { v: 'postmsg' as const, name: 'keyboard.postmsg.name', eng: 'PostMessage', rec: 'keyboard.postmsg.rec',
    desc: 'keyboard.postmsg.desc' },
  { v: 'sendmsg' as const, name: 'keyboard.sendmsg.name', eng: 'WinAPI', rec: 'keyboard.sendmsg.rec',
    desc: 'keyboard.sendmsg.desc' },
  { v: 'seize' as const, name: 'keyboard.seize.name', eng: 'SendInput', rec: 'keyboard.seize.rec',
    desc: 'keyboard.seize.desc' },
]

export const MOUSE_METHOD: Record<string, string> = {
  seize: 'sendinput', semi: 'sendinput', background: 'postmessage',
}

export const KEY_METHOD: Record<string, string> = {
  seize: 'sendinput', postmsg: 'postmessage', sendmsg: 'winapi',
}

// ── Key code → display name (shared: SettingsView recording + MonitorView matching) ──
export function codeToName(code: string): string {
  if (code.startsWith('Key')) return code.slice(3)          // KeyA → A
  if (code.startsWith('Digit')) return code.slice(5)         // Digit1 → 1
  if (code === 'Space') return 'Space'
  if (code.startsWith('F') && /^F\d+$/.test(code)) return code
  const m: Record<string, string> = {
    'ControlLeft': 'Ctrl', 'ControlRight': 'Ctrl',
    'AltLeft': 'Alt', 'AltRight': 'Alt',
    'ShiftLeft': 'Shift', 'ShiftRight': 'Shift',
    'MetaLeft': 'Win', 'MetaRight': 'Win',
    'Escape': 'Esc', 'Backspace': 'Backspace', 'Delete': 'Del',
    'Insert': 'Ins', 'Home': 'Home', 'End': 'End',
    'PageUp': 'PgUp', 'PageDown': 'PgDn',
    'Tab': 'Tab', 'CapsLock': 'CapsLock', 'Enter': 'Enter',
    'ArrowUp': '↑', 'ArrowDown': '↓', 'ArrowLeft': '←', 'ArrowRight': '→',
    'PrintScreen': 'PrtSc', 'ScrollLock': 'ScrlLk', 'Pause': 'Pause',
    'NumLock': 'NumLk', 'ContextMenu': 'Menu',
    'Minus': '-', 'Equal': '=', 'BracketLeft': '[', 'BracketRight': ']',
    'Backslash': '\\', 'Semicolon': ';', 'Quote': "'",
    'Comma': ',', 'Period': '.', 'Slash': '/', 'Backquote': '`',
  }
  return m[code] || code
}
