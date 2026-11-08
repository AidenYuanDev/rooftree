// Starts a blog server for the API tests: a fresh database (dbmate drop + up), a config generated
// from blog.yaml.dist, the admin created with --create-admin, mails written to an outbox directory.
//
// Environment:
//   BLOG_BIN            the server binary (default ../../build/bin/blog)
//   TEST_DATABASE_URL   postgres://user:password@host:port/name?sslmode=disable of a database the tests may drop
//                       (default postgres://postgres:postgres@127.0.0.1:5432/rooftree_blog_test?sslmode=disable)
//   TEST_PORT           default 18100

import { spawn, execFileSync } from 'node:child_process'
import { createHmac } from 'node:crypto'
import { appendFileSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const here = dirname(fileURLToPath(import.meta.url))
const blogDir = resolve(here, '..')
const blogBin = resolve(process.env.BLOG_BIN ?? join(blogDir, '../../build/bin/blog'))
const databaseUrl = process.env.TEST_DATABASE_URL ?? 'postgres://postgres:postgres@127.0.0.1:5432/rooftree_blog_test?sslmode=disable'
const port = Number(process.env.TEST_PORT ?? 18100)

export const origin = 'http://localhost:5173'
export const baseUrl = `http://127.0.0.1:${port}`
// The WebSocket, on the API's own port
export const wsUrl = `ws://127.0.0.1:${port}/ws`
export const adminPassword = 'admin password for tests'

function dbmate(...args) {
  execFileSync(join(blogDir, 'node_modules/.bin/dbmate'), ['--migrations-dir', join(blogDir, 'db/migrations'), '--no-dump-schema', ...args], {
    env: { ...process.env, DATABASE_URL: databaseUrl },
    stdio: 'pipe',
  })
}

function writeConfig(dir, { origins, captcha, mailInterval, oauth }) {
  const url = new URL(databaseUrl)
  const replacements = [
    [/User: ""\n    Password: ""\n    Name: "rooftree_blog"/, `User: "${decodeURIComponent(url.username)}"\n    Password: "${decodeURIComponent(url.password)}"\n    Name: "${url.pathname.slice(1)}"`],
    [/Host: "127.0.0.1"\n    Port: 5432/, `Host: "${url.hostname}"\n    Port: ${url.port || 5432}`],
    [/MigrationsDirectory: "blog-migrations"/, `MigrationsDirectory: "${join(blogDir, 'db/migrations')}"`],
    [/Port: 8100/, `Port: ${port}`],
    [/AllowedOrigins: \[\]/, `AllowedOrigins: ${JSON.stringify(origins)}`],
    [/CookieSecure: true/, 'CookieSecure: false'],
    [/OutboxDirectory: ""/, `OutboxDirectory: "${join(dir, 'outbox')}"`],
    // Fast hashes, the tests hash a lot
    [/Iterations: 2\n      MemoryCost: 19456/, 'Iterations: 1\n      MemoryCost: 8192'],
    [/LoginFailuresPerAccount: 10/, 'LoginFailuresPerAccount: 3'],
    [/MailInterval: 60/, `MailInterval: ${mailInterval}`],
    // A script can't read the picture: off but for captcha.test.mjs
    [/Captcha:\n    Enable: true/, `Captcha:\n    Enable: ${captcha}`],
    // Every test client comes from 127.0.0.1, which shares one session and its limits
    [/auth\.register: \{ Window: 3600, MaxCount: 20 \}/, 'auth.register: { Window: 3600, MaxCount: 1000 }'],
    [/auth\.login: \{ Window: 60, MaxCount: 10 \}/, 'auth.login: { Window: 60, MaxCount: 1000 }'],
    [/auth\.token: \{ Window: 60, MaxCount: 10 \}/, 'auth.token: { Window: 60, MaxCount: 1000 }'],
    [/auth\.mail: \{ Window: 3600, MaxCount: 10 \}/, 'auth.mail: { Window: 3600, MaxCount: 1000 }'],
    [/auth\.password: \{ Window: 60, MaxCount: 5 \}/, 'auth.password: { Window: 60, MaxCount: 1000 }'],
    [/auth\.totp: \{ Window: 60, MaxCount: 10 \}/, 'auth.totp: { Window: 60, MaxCount: 1000 }'],
    [/MaxQueuedMessagesPerSession: 64/, 'MaxQueuedMessagesPerSession: 1000'],
    [/posts\.search: \{ Window: 60, MaxCount: 30 \}/, 'posts.search: { Window: 60, MaxCount: 1000 }'],
    [/comments\.write: \{ Window: 60, MaxCount: 20 \}/, 'comments.write: { Window: 60, MaxCount: 1000 }'],
    // Small enough to reach in a test
    [/MaxPerUserPerPost: 1024/, 'MaxPerUserPerPost: 5'],
    [/PerUser: \{ Window: 60, MaxCount: 3 \}/, 'PerUser: { Window: 60, MaxCount: 10 }'],
    [/Directory: "uploads"/, `Directory: "${join(dir, 'uploads')}"`],
    // Drives small enough to fill in a test
    [/Directory: "drive"\n    Quota: 0\n    AdminQuota: 0\n    MaxFileSize: 2147483648\n    MinFreeSpace: 2147483648/,
      `Directory: "${join(dir, 'drive')}"\n    Quota: 0\n    AdminQuota: 0\n    MaxFileSize: 60000\n    MinFreeSpace: 0`],
    [/MailsPerMinute: 60/, 'MailsPerMinute: 60000'],
    // Rebuilds only leave a line in a file
    [/Command: ""\n    Debounce: 10/, `Command: "echo built >> ${join(dir, 'rebuilds')}"\n    Debounce: 1`],
    // A status sample a second, not a minute
    [/SampleInterval: 60/, 'SampleInterval: 1'],
    [/Level: 3\n    Appenders: \[Console\]/, 'Level: 4\n    Appenders: [Console]'],
    [/auth\.oauth: \{ Window: 60, MaxCount: 20 \}/, 'auth.oauth: { Window: 60, MaxCount: 1000 }'],
    // GitHub and Google: a fake provider's endpoints (oauth.test.mjs), else off
    [/  OAuth:\n    GitHub:\n      ClientId: ""\n      ClientSecret: ""\n    Google:\n      ClientId: ""\n      ClientSecret: ""/,
      oauth ? `  OAuth:\n${['GitHub', 'Google'].map((name) => {
        const base = `${oauth}/${name.toLowerCase()}`
        return `    ${name}:\n      ClientId: "${name.toLowerCase()}-client"\n      ClientSecret: "${name.toLowerCase()}-secret"\n` +
          `      AuthorizeUrl: "${base}/authorize"\n      TokenUrl: "${base}/token"\n      UserUrl: "${base}/user"\n      EmailsUrl: "${base}/emails"`
      }).join('\n')}` : '$&'],
    // Stopped at the end of a test file: right away, not after the restart notices
    [/ShutdownDelay: 30/, 'ShutdownDelay: 0'],
    // Its own console socket, not the dev server's
    [/Socket: "console\.sock"/, `Socket: "${join(dir, 'console.sock')}"`],
  ]

  let config = readFileSync(join(blogDir, 'blog.yaml.dist'), 'utf8')
  for (const [pattern, replacement] of replacements) {
    if (!pattern.test(config))
      throw new Error(`blog.yaml.dist changed, ${pattern} no longer matches`)
    config = config.replace(pattern, replacement)
  }

  const file = join(dir, 'blog.yaml')
  writeFileSync(file, config)
  return file
}

export function totp(base32Secret, offset = 0) {
  const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'
  let bits = ''
  for (const c of base32Secret.toUpperCase())
    bits += alphabet.indexOf(c).toString(2).padStart(5, '0')
  const secret = Buffer.from(bits.match(/.{8}/g).map((b) => parseInt(b, 2)))

  const counter = Buffer.alloc(8)
  counter.writeBigUInt64BE(BigInt(Math.floor(Date.now() / 30000) + offset))
  const digest = createHmac('sha1', secret).update(counter).digest()
  const offsetByte = digest[19] & 15
  return String((digest.readUInt32BE(offsetByte) & 0x7fffffff) % 1000000).padStart(6, '0')
}

// origins: where the browser pages of the test come from (the vite dev server of the e2e tests).
// captcha, mailInterval: Blog.Captcha.Enable and Blog.Auth.MailInterval, off for most tests.
// oauth: the address of a fake GitHub and Google (oauth.test.mjs), none for most tests
export async function startServer({ origins = [origin], captcha = false, mailInterval = 0, oauth } = {}) {
  const dir = mkdtempSync(join(tmpdir(), 'blog-api-test-'))
  try { dbmate('drop') } catch { /* not there yet */ }
  dbmate('up')
  const configFile = writeConfig(dir, { origins, captcha, mailInterval, oauth })

  const created = execFileSync(blogBin, ['--config', configFile, '--create-admin', '--username', 'author', '--email', 'author@example.com',
    '--display-name', 'The Author', '--totp'], { env: { ...process.env, BLOG_ADMIN_PASSWORD: adminPassword }, encoding: 'utf8' })
  const adminTotpSecret = created.match(/by hand: ([A-Z2-7]+)/)[1]

  // Also in tests/.server.log, the last run's, for when a test fails
  const logFile = join(here, '.server.log')
  writeFileSync(logFile, '')
  const log = []
  const child = spawn(blogBin, ['--config', configFile], { stdio: ['ignore', 'pipe', 'pipe'] })
  const record = (data) => {
    log.push(data.toString())
    appendFileSync(logFile, data)
  }
  child.stdout.on('data', record)
  child.stderr.on('data', record)
  child.on('exit', (code, signal) => appendFileSync(logFile, `\n[server exited: code ${code}, signal ${signal}]\n`))

  for (let i = 0; ; ++i) {
    try {
      if ((await fetch(`${baseUrl}/api/auth/me`)).ok)
        break
    } catch {
      if (i > 100 || child.exitCode !== null)
        throw new Error(`the server did not start:\n${log.join('')}`)
      await new Promise((r) => setTimeout(r, 50))
    }
  }

  return {
    adminTotpSecret,
    log,
    outbox: join(dir, 'outbox'),
    rebuildsFile: join(dir, 'rebuilds'),
    // The console's output for lines of commands, as from a pipe
    console(...lines) {
      return execFileSync(blogBin, ['--config', configFile, '--console', '--socket', join(dir, 'console.sock')],
        { input: lines.join('\n') + '\n', encoding: 'utf8' })
    },
    async stop() {
      child.kill('SIGTERM')
      await new Promise((r) => child.once('exit', r))
      rmSync(dir, { recursive: true, force: true })
    },
  }
}

// The newest mail to an address: its text body and the token of its link
export function lastMailTo(outbox, address) {
  let files = []
  try { files = readdirSync(outbox).sort() } catch { /* no mail yet */ }
  for (const name of files.reverse()) {
    const mail = readFileSync(join(outbox, name), 'utf8')
    if (!mail.includes(`To: <${address}>`))
      continue
    const encoded = mail.split('\r\n\r\n').slice(1).join('\r\n\r\n').replace(/\r\n/g, '')
    const text = Buffer.from(encoded, 'base64').toString('utf8')
    return { mail, text, token: text.match(/token=([A-Za-z0-9_-]+)/)?.[1] }
  }
  return undefined
}

// A browser: keeps its cookies and the CSRF token of its login, sends our Origin
export class Client {
  cookies = new Map()
  csrfToken = null

  async request(method, path, body, headers = {}) {
    // A FormData body goes as multipart, fetch sets its Content-Type
    const form = body instanceof FormData
    const response = await fetch(`${baseUrl}${path}`, {
      method,
      redirect: 'manual',
      headers: {
        Origin: origin,
        ...(body !== undefined && !form ? { 'Content-Type': 'application/json' } : {}),
        ...(this.csrfToken ? { 'X-CSRF-Token': this.csrfToken } : {}),
        ...(this.cookies.size ? { Cookie: [...this.cookies].map(([k, v]) => `${k}=${v}`).join('; ') } : {}),
        ...headers,
      },
      body: body === undefined || form ? body : typeof body === 'string' ? body : JSON.stringify(body),
    })

    for (const cookie of response.headers.getSetCookie()) {
      const [pair, ...attributes] = cookie.split(';').map((s) => s.trim())
      const [name, value] = pair.split('=')
      if (attributes.some((a) => /^Max-Age=0$/i.test(a)) || value === '')
        this.cookies.delete(name)
      else
        this.cookies.set(name, value)
    }

    const text = await response.text()
    const json = text ? JSON.parse(text) : null
    if (json && typeof json.csrfToken !== 'undefined')
      this.csrfToken = json.csrfToken
    return { status: response.status, json, headers: response.headers }
  }

  get(path, headers) { return this.request('GET', path, undefined, headers) }
  post(path, body, headers) { return this.request('POST', path, body ?? {}, headers) }
  patch(path, body, headers) { return this.request('PATCH', path, body, headers) }
  put(path, body, headers) { return this.request('PUT', path, body, headers) }
  delete(path, headers) { return this.request('DELETE', path, undefined, headers) }
  // Bytes as the body, the answer's bytes back (a drive's pieces and downloads)
  async binary(method, path, body, headers = {}) {
    const response = await fetch(`${baseUrl}${path}`, {
      method,
      headers: {
        Origin: origin,
        ...(body !== undefined ? { 'Content-Type': 'application/octet-stream' } : {}),
        ...(this.csrfToken ? { 'X-CSRF-Token': this.csrfToken } : {}),
        ...(this.cookies.size ? { Cookie: [...this.cookies].map(([k, v]) => `${k}=${v}`).join('; ') } : {}),
        ...headers,
      },
      body,
    })
    const data = Buffer.from(await response.arrayBuffer())
    const json = response.headers.get('content-type')?.startsWith('application/json') ? JSON.parse(data.toString()) : null
    return { status: response.status, data, json, headers: response.headers }
  }

  // A file as the multipart field "file"
  upload(path, data, name = 'file', type = 'application/octet-stream') {
    const form = new FormData()
    form.append('file', new Blob([data], { type }), name)
    return this.request('POST', path, form)
  }
}
