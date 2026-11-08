import { fileURLToPath } from 'node:url'
import { defineConfig } from 'vitest/config'

// Component and unit tests: jsdom, the API mocked with MSW (tests/msw.ts). Not the React Router plugin,
// tests render route components inside createRoutesStub
export default defineConfig({
  resolve: {
    alias: { '~': fileURLToPath(new URL('./app', import.meta.url)) },
  },
  define: { __PUBLIC_ORIGIN__: JSON.stringify('https://blog.example.com') },
  test: {
    environment: 'jsdom',
    globals: true,
    setupFiles: ['./tests/setup.ts'],
    include: ['app/**/*.test.{ts,tsx}', 'tests/**/*.test.{ts,tsx}'],
  },
})
