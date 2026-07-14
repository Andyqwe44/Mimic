// ═══ Vite config — dev server for WebView2 HMR ═══
import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'

// Single source of truth (铁律 8): parse the version from version.h so the
// splash can show it instantly, before the runtime get_version round-trip.
const versionHeader = readFileSync(
  fileURLToPath(new URL('../monitor_app/src/version.h', import.meta.url)),
  'utf8',
)
const appVersion = versionHeader.match(/APP_VERSION\s+"([^"]+)"/)?.[1] ?? '0.0.0'

export default defineConfig({
  plugins: [react(), tailwindcss()],

  // Compile-time constant — see version.h parse above.
  define: {
    __APP_VERSION__: JSON.stringify(appVersion),
  },

  // ── Dev server — fixed port 1420, C++ host navigates to localhost:1420 ──
  server: {
    port: 1420,
    strictPort: true, // fail if port occupied (never silently switch)
    // Explicit ws protocol required for WebView2 HMR compatibility
    hmr: {
      protocol: 'ws',
      host: 'localhost',
    },
    // Watch version.h + settings.default.json so frontend picks up changes
    // without manual dev-server restart (铁律 8: version.h is single source).
    watch: {
      ignored: ['!**/monitor_app/src/version.h', '!**/monitor_app/build/config/settings.default.json'],
    },
  },

  clearScreen: false,              // keep Vite log output visible
  envPrefix: ['VITE_', 'TAURI_'],  // expose VITE_* + legacy TAURI_* env vars
})
