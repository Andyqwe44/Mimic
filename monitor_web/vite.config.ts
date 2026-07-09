// ═══ Vite config — dev server for WebView2 HMR ═══
import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

export default defineConfig({
  plugins: [react(), tailwindcss()],

  // ── Dev server — fixed port 1420, C++ host navigates to localhost:1420 ──
  server: {
    port: 1420,
    strictPort: true, // fail if port occupied (never silently switch)
    // Explicit ws protocol required for WebView2 HMR compatibility
    hmr: {
      protocol: 'ws',
      host: 'localhost',
    },
  },

  clearScreen: false,              // keep Vite log output visible
  envPrefix: ['VITE_', 'TAURI_'],  // expose VITE_* + legacy TAURI_* env vars
})
