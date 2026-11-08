// Reads of the site: the pages report them, the admin sees them per day, page and referrer
// (pnpm test:api)

import assert from 'node:assert/strict'
import { after, before, describe, test } from 'node:test'
import { adminPassword, Client, startServer, totp } from './server.mjs'

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

const firefox = { 'User-Agent': 'Mozilla/5.0 (X11; Linux x86_64; rv:140.0) Gecko/20100101 Firefox/140.0' }
const chrome = { 'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/140.0 Safari/537.36' }

describe('reads', () => {
  test('counted per page and day, a reader once a day; bots, private pages and our own links not', async () => {
    const reader = new Client()
    const view = (path, referrer, headers = firefox) => reader.post('/api/views', { path, referrer }, headers)

    assert.equal((await view('/zh/posts/hello', 'https://www.google.com/search?q=x')).status, 200)
    // Ours (Blog.PublicOrigin): no referrer
    await view('/zh/posts/hello/', 'http://localhost:5173/zh')
    await view('/zh/posts/hello', '', chrome)
    await view('/zh', 'https://news.ycombinator.com/item?id=1')
    // Not counted
    await view('/zh/posts/hello', 'https://www.google.com/', { 'User-Agent': 'Mozilla/5.0 (compatible; Googlebot/2.1)' })
    await view('/zh/admin/stats', '')
    await view('/en/drive', '')
    await view('/zh/s/abc', '')
    await view('/fr/posts/hello', '')
    await view('/zh/posts/hello?x=1', '')
    await view('https://elsewhere.example/zh', '')
    assert.equal((await reader.post('/api/views', { path: '/zh' }, { ...firefox, Origin: 'https://evil.example' })).status, 403)

    assert.equal((await reader.get('/api/admin/stats')).status, 401)
    const stats = (await admin.get('/api/admin/stats?days=7')).json
    assert.equal(stats.days.length, 7)
    assert.equal(stats.days.at(-1).day, stats.today)
    assert.deepEqual(stats.days.at(-1), { day: stats.today, views: 4, visitors: 2 })
    assert.deepEqual(stats.days[0].views, 0)
    assert.deepEqual(stats.pages, [
      { path: '/zh/posts/hello', views: 3, visitors: 2 },
      { path: '/zh', views: 1, visitors: 1 },
    ])
    assert.deepEqual(stats.referrers, [
      { host: 'news.ycombinator.com', views: 1 },
      { host: 'www.google.com', views: 1 },
    ])

    // Written once: asked again, nothing is added twice
    await view('/zh/posts/hello', '')
    const again = (await admin.get('/api/admin/stats?days=1')).json
    assert.deepEqual(again.days, [{ day: stats.today, views: 5, visitors: 2 }])
    assert.equal(again.pages[0].views, 4)
  })
})
