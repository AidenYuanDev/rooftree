// The WebSocket (readers per page, notices of new writing) and mail subscriptions (pnpm test:api)

import assert from 'node:assert/strict'
import { after, before, describe, test } from 'node:test'
import { readdirSync, readFileSync } from 'node:fs'
import { join } from 'node:path'
import { adminPassword, baseUrl, Client, lastMailTo, origin, startServer, totp, wsUrl } from './server.mjs'

let server
let admin

before(async () => {
  server = await startServer()
  admin = new Client()
  assert.equal((await admin.post('/api/auth/login', { login: 'author', password: adminPassword, totp: totp(server.adminTotpSecret) })).status, 200)
})

after(async () => {
  if (process.exitCode && server)
    console.log(server.log.join(''))
  await server?.stop()
})

// A connection that keeps what it receives; next(predicate) waits for a message
function connect(headers = { Origin: origin }) {
  const socket = new WebSocket(wsUrl, { headers })
  const received = []
  const waiting = []
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data)
    received.push(message)
    for (const waiter of [...waiting]) {
      if (waiter.predicate(message)) {
        waiting.splice(waiting.indexOf(waiter), 1)
        waiter.resolve(message)
      }
    }
  })
  const opened = new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve)
    socket.addEventListener('error', reject)
  })
  return {
    socket,
    opened,
    received,
    send: (message) => socket.send(JSON.stringify(message)),
    next: (predicate, timeout = 4000) => {
      const found = received.find(predicate)
      if (found) return Promise.resolve(found)
      return new Promise((resolve, reject) => {
        waiting.push({ predicate, resolve })
        setTimeout(() => reject(new Error(`no such message, got ${JSON.stringify(received)}`)), timeout)
      })
    },
  }
}

describe('websocket', () => {
  test('readers per page and on the site', async () => {
    const cookie = [...admin.cookies].map(([k, v]) => `${k}=${v}`).join('; ')
    // Every test client is 127.0.0.1: the ones logged out are one reader, the author another
    const a = connect()
    const b = connect()
    const author = connect({ Origin: origin, Cookie: cookie })
    const authorAgain = connect({ Origin: origin, Cookie: cookie })
    await Promise.all([a.opened, b.opened, author.opened, authorAgain.opened])
    a.send({ type: 'page', path: '/zh/posts/hello' })
    b.send({ type: 'page', path: '/zh/posts/hello?from=rss#comments' })
    author.send({ type: 'page', path: '/zh/posts/hello' })
    authorAgain.send({ type: 'page', path: '/en/notes' })

    // Four pages, two readers
    assert.ok(await a.next((m) => m.type === 'presence' && m.page === 2 && m.site === 2))
    assert.ok(await authorAgain.next((m) => m.type === 'presence' && m.page === 1 && m.site === 2))

    // The author's other tab still counts them; leaving the post, they aren't on it
    author.socket.close()
    assert.ok(await a.next((m) => m.type === 'presence' && m.page === 1 && m.site === 2))
    authorAgain.socket.close()
    assert.ok(await b.next((m) => m.type === 'presence' && m.page === 1 && m.site === 1))

    const invalid = connect()
    await invalid.opened
    invalid.send({ type: 'page', path: 'not a path' })
    assert.equal((await invalid.next((m) => m.type === 'error')).code, 'invalid_path')

    for (const client of [a, b, invalid]) client.socket.close()
  })

  test('new writing is announced', async () => {
    const reader = connect()
    await reader.opened
    reader.send({ type: 'page', path: '/zh' })
    await reader.next((m) => m.type === 'presence')

    const note = await admin.post('/api/admin/notes', { status: 'published', originalLang: 'zh', translations: { zh: { title: '刚写的', body: '新的' } } })
    assert.deepEqual(await reader.next((m) => m.type === 'published'), { type: 'published', kind: 'note', path: `/notes/${note.json.id}`, title: '刚写的' })

    // A draft isn't
    await admin.post('/api/admin/posts', { slug: 'secret', status: 'draft', originalLang: 'en', translations: { en: { title: 'Secret', body: 'x' } } })
    const post = await admin.post('/api/admin/posts', { slug: 'public', status: 'published', originalLang: 'en', translations: { en: { title: 'Public', body: 'x' } } })
    assert.equal(post.status, 201)
    const announced = await reader.next((m) => m.type === 'published' && m.kind === 'post')
    assert.equal(announced.path, '/posts/public')
    reader.socket.close()
  })

  test('watched targets announce comments, reactions and republished text once per refresh', async () => {
    const post = (
      await admin.post('/api/admin/posts', {
        slug: 'watched',
        status: 'published',
        originalLang: 'en',
        translations: { en: { title: 'Watched', body: 'First paragraph to quote.\n\nSecond paragraph.' } },
      })
    ).json
    const target = `post:${post.id}`

    const created = await admin.post('/api/admin/users', {
      username: 'commenter_live',
      displayName: 'Live',
      email: 'commenter-live@example.com',
      password: 'password live',
      emailVerified: true,
    })
    assert.equal(created.status, 201)
    const commenter = new Client()
    assert.equal((await commenter.post('/api/auth/login', { login: 'commenter_live', password: 'password live' })).status, 200)

    const watching = connect()
    const elsewhere = connect()
    await Promise.all([watching.opened, elsewhere.opened])
    watching.send({ type: 'page', path: '/en/posts/watched', watch: [target] })
    elsewhere.send({ type: 'page', path: '/en/guestbook', watch: ['guestbook'] })
    await Promise.all([watching.next((m) => m.type === 'presence'), elsewhere.next((m) => m.type === 'presence')])
    watching.received.length = 0
    elsewhere.received.length = 0

    // Two comments in the same refresh coalesce into one notice, only for the page watching the post
    const first = await commenter.post('/api/comments', {
      target,
      body: 'on the passage',
      anchor: { lang: 'en', block: 0, quote: 'First paragraph to quote.', prefix: '', suffix: '', text: 'First paragraph to quote.' },
    })
    assert.equal(first.status, 201)
    assert.equal((await commenter.post('/api/comments', { target, body: 'and a second' })).status, 201)
    assert.deepEqual(await watching.next((m) => m.type === 'changed'), { type: 'changed', target, what: 'comments' })
    assert.equal(watching.received.filter((m) => m.type === 'changed' && m.what === 'comments').length, 1)
    assert.equal(elsewhere.received.filter((m) => m.type === 'changed').length, 0)

    // A reaction on a comment of the post notifies the post's watchers
    watching.received.length = 0
    assert.equal((await commenter.post('/api/reactions', { target: `comment:${first.json.id}`, kind: 'heart', on: true })).status, 200)
    assert.deepEqual(await watching.next((m) => m.type === 'changed'), { type: 'changed', target, what: 'reactions' })

    // Republishing without the quoted sentence notifies both content and comments (the passage became orphaned)
    watching.received.length = 0
    assert.equal(
      (await admin.patch(`/api/admin/posts/${post.id}`, { translations: { en: { title: 'Watched v2', summary: '', body: 'Second paragraph only.' } } }))
        .status,
      200,
    )
    assert.ok(await watching.next((m) => m.type === 'changed' && m.target === target && m.what === 'content'))
    assert.ok(await watching.next((m) => m.type === 'changed' && m.target === target && m.what === 'comments'))

    // Purging the commenter notifies every watching page ("*")
    watching.received.length = 0
    elsewhere.received.length = 0
    assert.equal((await admin.delete(`/api/admin/users/${created.json.id}?mode=purge`)).status, 200)
    assert.ok(await watching.next((m) => m.type === 'changed' && m.target === '*' && m.what === 'comments'))
    assert.ok(await watching.next((m) => m.type === 'changed' && m.target === '*' && m.what === 'reactions'))
    assert.ok(await elsewhere.next((m) => m.type === 'changed' && m.target === '*' && m.what === 'comments'))

    watching.socket.close()
    elsewhere.socket.close()
  })

  test('only from our pages', async () => {
    const stranger = connect({ Origin: 'https://evil.example.com' })
    await assert.rejects(stranger.opened)
  })
})

describe('status', () => {
  test('the server\'s numbers, for the admin only', async () => {
    assert.equal((await new Client().get('/api/status')).status, 401)

    const reader = new Client()
    await reader.post('/api/auth/register', { username: 'reader_status', displayName: 'S', email: 'reader-status@example.com', password: 'password reader' })
    assert.equal((await reader.post('/api/auth/login', { login: 'reader_status', password: 'password reader' })).status, 200)
    assert.equal((await reader.get('/api/status')).status, 403)

    // A reader on a page, and a few samples (one a second in the tests)
    const page = connect()
    await page.opened
    page.send({ type: 'page', path: '/zh/status' })
    await page.next((m) => m.type === 'presence')
    await new Promise((resolve) => setTimeout(resolve, 2500))

    const status = await admin.get('/api/status')
    assert.equal(status.status, 200)
    const { now, interval, series } = status.json
    assert.equal(interval, 1)
    assert.ok(now.uptime >= 2)
    assert.equal(now.readers, 1)
    assert.equal(now.pages, 1)
    assert.ok(now.cores >= 1)
    assert.ok(now.memoryMiB > 0 && now.diskFreePercent > 0 && now.diskFreePercent <= 100)
    assert.ok(series.at.length >= 2)
    for (const values of Object.values(series)) assert.equal(values.length, series.at.length)
    assert.ok(Math.max(...series.requests) > 0)
    assert.ok(Math.max(...series.readers) >= 1)
    // Nothing about where the server is
    assert.doesNotMatch(JSON.stringify(status.json), /127\.0\.0\.1|\/tmp|uploads/)

    // Only what came after since
    const last = series.at.at(-1)
    const later = await admin.get(`/api/status?since=${last}`)
    assert.ok(later.json.series.at.every((at) => at > last))
    page.socket.close()
  })
})

// Every mail to address, decoded
function mailsTo(address) {
  let files = []
  try { files = readdirSync(server.outbox).sort() } catch { /* none yet */ }
  return files.map((name) => readFileSync(join(server.outbox, name), 'utf8')).filter((mail) => mail.includes(`To: <${address}>`))
    .map((mail) => ({ mail, text: Buffer.from(mail.split('\r\n\r\n').slice(1).join('').replace(/\r\n/g, ''), 'base64').toString('utf8') }))
}

async function waitFor(check, timeout = 4000) {
  for (const start = Date.now(); Date.now() - start < timeout; await new Promise((r) => setTimeout(r, 50)))
    if (check()) return
  assert.fail('timed out')
}

describe('subscriptions', () => {
  test('double opt-in, newsletters, unsubscribing', async () => {
    const anon = new Client()
    assert.equal((await anon.post('/api/subscribe', { email: 'nope' })).json.error.fields.email, 'invalid')
    assert.equal((await anon.post('/api/subscribe', { email: 'Fan@Example.com', lang: 'en' })).status, 200)
    const confirm = lastMailTo(server.outbox, 'fan@example.com')
    assert.match(confirm.text, /\/en\/subscribe\/confirm\?token=/)

    // Not yet confirmed: not mailed
    const first = await admin.post('/api/admin/posts', { slug: 'before-confirm', status: 'published', originalLang: 'en', translations: { en: { title: 'Early', body: 'x' } } })
    assert.equal(first.status, 201)

    assert.equal((await anon.post('/api/subscribe/confirm', { token: 'made-up' })).json.error.code, 'invalid_token')
    assert.equal((await anon.post('/api/subscribe/confirm', { token: confirm.token })).status, 200)
    assert.equal((await anon.post('/api/subscribe/confirm', { token: confirm.token })).json.error.code, 'invalid_token')
    // Subscribing again changes nothing and sends nothing
    const before = mailsTo('fan@example.com').length
    assert.equal((await anon.post('/api/subscribe', { email: 'fan@example.com' })).status, 200)
    assert.equal(mailsTo('fan@example.com').length, before)

    await admin.post('/api/admin/posts', { slug: 'mailed', status: 'published', originalLang: 'zh',
      translations: { zh: { title: '中文标题', summary: '中文摘要', body: 'x' }, en: { title: 'English title', summary: 'In English', body: 'x' } } })
    await waitFor(() => mailsTo('fan@example.com').some((m) => m.text.includes('English title')))
    const newsletter = mailsTo('fan@example.com').find((m) => m.text.includes('English title'))
    assert.match(newsletter.text, /In English/)
    assert.match(newsletter.text, /http:\/\/localhost:5173\/en\/posts\/mailed/)
    assert.match(newsletter.mail, /List-Unsubscribe: <http:\/\/localhost:5173\/api\/unsubscribe\?token=[\w-]{43}>/)
    assert.match(newsletter.mail, /List-Unsubscribe-Post: List-Unsubscribe=One-Click/)
    assert.ok(!mailsTo('fan@example.com').some((m) => m.text.includes('Early')))

    // Notes only when asked, and only to the readers of its languages
    const both = (title) => ({ zh: { title, body: 'x' }, en: { title, body: 'x' } })
    await admin.post('/api/admin/notes', { status: 'published', originalLang: 'zh', translations: both('A quiet note') })
    await admin.post('/api/admin/notes', { status: 'published', originalLang: 'zh', notify: true, translations: { zh: { title: '只有中文的日记', body: 'x' } } })
    await admin.post('/api/admin/notes', { status: 'published', originalLang: 'zh', notify: true, translations: both('A note to mail') })
    await waitFor(() => mailsTo('fan@example.com').some((m) => m.text.includes('A note to mail')))
    assert.ok(!mailsTo('fan@example.com').some((m) => m.text.includes('A quiet note') || m.text.includes('只有中文的日记')))

    // The mail client's one-click unsubscribe: a POST with the token, no Origin of ours
    const token = newsletter.mail.match(/unsubscribe\?token=([\w-]{43})/)[1]
    const oneClick = await fetch(`${baseUrl}/api/unsubscribe?token=${token}`, { method: 'POST', body: 'List-Unsubscribe=One-Click',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' } })
    assert.equal(oneClick.status, 200)
    const count = mailsTo('fan@example.com').length
    await admin.post('/api/admin/posts', { slug: 'after-unsubscribe', status: 'published', originalLang: 'en', translations: { en: { title: 'Late', body: 'x' } } })
    await new Promise((r) => setTimeout(r, 300))
    assert.equal(mailsTo('fan@example.com').length, count)
    assert.equal((await anon.post('/api/unsubscribe', { token: 'x'.repeat(43) })).json.error.code, 'invalid_token')
  })

  test('logged in readers switch it in their account', async () => {
    const reader = new Client()
    const email = 'reader-sub@example.com'
    await reader.post('/api/auth/register', { username: 'reader_sub', displayName: 'R', email, password: 'password reader' })
    assert.equal((await reader.post('/api/auth/login', { login: 'reader_sub', password: 'password reader' })).status, 200)
    assert.equal((await reader.get('/api/auth/subscription')).json.subscribed, false)
    // The address has to be confirmed first
    assert.equal((await reader.put('/api/auth/subscription', { subscribed: true })).json.error.code, 'email_unverified')
    await reader.post('/api/auth/verify-email', { token: lastMailTo(server.outbox, email).token })
    assert.equal((await reader.put('/api/auth/subscription', { subscribed: true })).json.subscribed, true)
    assert.equal((await reader.get('/api/auth/subscription')).json.subscribed, true)

    await admin.post('/api/admin/posts', { slug: 'for-readers', status: 'published', originalLang: 'zh', translations: { zh: { title: '给读者的', body: 'x' } } })
    await waitFor(() => mailsTo(email).some((m) => m.text.includes('给读者的')))
    assert.equal((await reader.put('/api/auth/subscription', { subscribed: false })).json.subscribed, false)
    assert.equal((await reader.get('/api/auth/subscription')).json.subscribed, false)
  })
})
