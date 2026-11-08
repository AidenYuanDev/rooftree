// CI's web build: prerendering reads the API, so this starts the blog server just built on a fresh
// database (apps/blog/tests/server.mjs, BLOG_BIN and TEST_DATABASE_URL as for the API tests), builds
// against it and stops it.
import { execFileSync } from 'node:child_process'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const blogDir = resolve(dirname(fileURLToPath(import.meta.url)), '../../apps/blog')
const { startServer, baseUrl } = await import(resolve(blogDir, 'tests/server.mjs'))

const server = await startServer()
try {
  execFileSync('pnpm', ['build:web'], { cwd: blogDir, stdio: 'inherit', env: { ...process.env, BLOG_API_URL: baseUrl } })
} finally {
  await server.stop()
}
