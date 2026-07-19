// Safe clipboard / share — Android WebView often rejects navigator.clipboard.
import { hostCall } from './bridge'
import { getHostPlatform } from './platform'

export async function copyText(text: string): Promise<boolean> {
  if (!text) return false

  // Prefer native clipboard on Android (avoids NotAllowedError).
  if (getHostPlatform() === 'android') {
    try {
      const r = await hostCall('clipboard_write', { text })
      if (r && r.ok !== false) return true
    } catch {
      /* fall through */
    }
  }

  try {
    if (navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(text)
      return true
    }
  } catch {
    /* fall through */
  }

  try {
    const ta = document.createElement('textarea')
    ta.value = text
    ta.setAttribute('readonly', '')
    ta.style.position = 'fixed'
    ta.style.left = '-9999px'
    ta.style.top = '0'
    document.body.appendChild(ta)
    ta.focus()
    ta.select()
    const ok = document.execCommand('copy')
    document.body.removeChild(ta)
    return ok
  } catch {
    return false
  }
}

/**
 * Share sheet (Android) — prefers a .txt file via FileProvider so QQ/WeChat
 * are not limited by EXTRA_TEXT length. Falls back to clipboard copy.
 */
export async function shareText(text: string, filename = 'mimic-log.txt'): Promise<boolean> {
  if (!text) return false
  if (getHostPlatform() === 'android') {
    try {
      const r = await hostCall('share_text', { text, filename, as_file: true })
      if (r && r.ok !== false) return true
    } catch {
      /* fall through */
    }
  }
  return copyText(text)
}

/** Pull full live log from host (Android ring + file). */
export async function exportLiveLog(): Promise<string> {
  try {
    if (getHostPlatform() === 'android') {
      const r = await hostCall('export_live_log')
      if (typeof r?.content === 'string' && r.content) return r.content
    }
    const res = await hostCall('read_live_log')
    const raw = typeof res === 'string' ? res : res?.lines || ''
    return raw
  } catch {
    return ''
  }
}
