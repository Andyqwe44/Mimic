// ═══ Vite config — production build for MimicClient WebView2 ═══
import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'

// Single source of truth (铁律 8): parse APP_VERSION from version.h
const versionHeader = readFileSync(
  fileURLToPath(new URL('../mimic_client/src/version.h', import.meta.url)),
  'utf8',
)
const appVersion = versionHeader.match(/APP_VERSION\s+"([^"]+)"/)?.[1] ?? '0.0.0'

export default defineConfig({
  plugins: [react(), tailwindcss()],
  define: {
    __APP_VERSION__: JSON.stringify(appVersion),
  },
  // Local preview only (optional). Release uses `npm run build` → dist/.
  server: {
    port: 1420,
    strictPort: true,
  },
  clearScreen: false,
})
