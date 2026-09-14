import tailwindcss from '@tailwindcss/vite'
import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'

// https://vite.dev/config/
export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    // Dev-time convenience: `npm run dev` proxies API calls to the running
    // game's admin server so the UI can be iterated on without rebuilding.
    // The built app (dist/) is served directly by that same admin server in
    // production, so no proxy is needed there.
    proxy: {
      '/api': 'http://127.0.0.1:27180',
    },
  },
})
