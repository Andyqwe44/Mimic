import type { CapacitorConfig } from '@capacitor/cli'

/**
 * Android host loads the same Vite build as Windows WebView2 (`shared/web/dist`).
 * Native bridge: MimicHost Capacitor plugin ↔ JS `hostCall`.
 */
const config: CapacitorConfig = {
  appId: 'com.mimic.client',
  appName: 'Mimic',
  webDir: '../shared/web/dist',
  server: {
    androidScheme: 'https',
  },
  android: {
    allowMixedContent: true,
  },
}

export default config
