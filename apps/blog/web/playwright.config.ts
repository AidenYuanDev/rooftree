import { defineConfig, devices } from '@playwright/test'

// End to end: a real API server on a fresh database (../tests/server.mjs, started in global-setup.ts)
// and the vite dev server in front of it. PLAYWRIGHT_CHANNEL=chrome uses an installed Chrome instead of
// Playwright's own Chromium (pnpm exec playwright install chromium)
const port = 5174
const origin = `http://127.0.0.1:${port}`

export default defineConfig({
  testDir: './e2e',
  fullyParallel: false,
  workers: 1,
  retries: process.env.CI ? 1 : 0,
  reporter: process.env.CI ? 'github' : 'list',
  globalSetup: './e2e/global-setup.ts',
  use: {
    baseURL: origin,
    trace: 'retain-on-failure',
    ...devices['Desktop Chrome'],
    channel: process.env.PLAYWRIGHT_CHANNEL,
  },
  webServer: {
    command: `pnpm exec react-router dev --host 127.0.0.1 --port ${port}`,
    url: `${origin}/zh/login`,
    reuseExistingServer: false,
    env: { BLOG_API_URL: 'http://127.0.0.1:18100', BLOG_PUBLIC_ORIGIN: origin },
    timeout: 60_000,
  },
})
