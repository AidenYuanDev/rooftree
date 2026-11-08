import { fileURLToPath } from 'node:url'
import { reactRouter } from '@react-router/dev/vite'
import tailwindcss from '@tailwindcss/vite'
import { defineConfig } from 'vite'

// The API server the dev server forwards /api and its WebSocket (/ws, the same port) to
const api = process.env.BLOG_API_URL ?? 'http://127.0.0.1:8100'
const webSocket = api.replace(/^http/, 'ws')

// Where readers open the site, for canonical URLs; the build sets it (deploy/build.sh)
const publicOrigin = process.env.BLOG_PUBLIC_ORIGIN ?? 'http://localhost:5173'

export default defineConfig({
  plugins: [tailwindcss(), reactRouter()],
  define: { __PUBLIC_ORIGIN__: JSON.stringify(publicOrigin) },
  resolve: {
    alias: { '~': fileURLToPath(new URL('./app', import.meta.url)) },
  },
  // Pre-bundle the dependencies of every page when the dev server starts: discovering them on the first
  // visit to each page reloads it, which breaks a page being used (and the e2e tests on a cold cache)
  optimizeDeps: { entries: ['./app/**/*.tsx'] },
  server: {
    port: 5173,
    strictPort: true,
    proxy: {
      '/api': { target: api, changeOrigin: false },
      '/uploads': { target: api, changeOrigin: false },
      '/ws': { target: webSocket, ws: true, changeOrigin: false },
      // The server maps these the same way in production (server/StaticHandlers.cpp)
      '^/(zh|en)/feed\\.xml$': { target: api, rewrite: (path) => `/api/feed.xml?lang=${path.slice(1, 3)}` },
      '/sitemap.xml': { target: api, rewrite: () => '/api/sitemap.xml' },
    },
  },
})
