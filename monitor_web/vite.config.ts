import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    port: 1420,
    strictPort: true,
    hmr: {
      protocol: 'ws',
      host: 'localhost',
    },
  },
  clearScreen: false,
  envPrefix: ['VITE_', 'TAURI_'],
})
