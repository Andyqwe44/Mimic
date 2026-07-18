/** Runtime host: Windows WebView2 vs Android Capacitor WebView (shared UI). */

export type HostPlatform = 'windows' | 'android' | 'browser'

declare global {
  interface Window {
    Capacitor?: {
      isNativePlatform?: () => boolean
      getPlatform?: () => string
      Plugins?: Record<string, any>
    }
  }
}

export function getHostPlatform(): HostPlatform {
  const cap = typeof window !== 'undefined' ? window.Capacitor : undefined
  if (cap?.isNativePlatform?.() || cap?.getPlatform?.() === 'android') return 'android'
  if (typeof window !== 'undefined' && (window as any).chrome?.webview) return 'windows'
  return 'browser'
}

export function isAndroidHost(): boolean {
  return getHostPlatform() === 'android'
}

export function isWindowsHost(): boolean {
  return getHostPlatform() === 'windows'
}
