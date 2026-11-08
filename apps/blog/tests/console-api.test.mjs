// The console's api command: any route, as the admin or another account, with JSON, items, files and
// forms, against a real server (pnpm test:api).

import assert from 'node:assert/strict'
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { after, before, test } from 'node:test'
import { Client, lastMailTo, startServer } from './server.mjs'

let server
let files

before(async () => {
  server = await startServer()
  files = mkdtempSync(join(tmpdir(), 'blog-console-api-'))
})

after(async () => {
  if (process.exitCode && server)
    console.log(server.log.join(''))
  await server?.stop()
  rmSync(files, { recursive: true, force: true })
})

// The JSON the console printed after its status line
function printedJson(output) {
  return JSON.parse(output.slice(output.indexOf('{')).replace(/\s*RT> ?$/, '').trim())
}

test('the routes are listed, a filter narrows them', () => {
  const all = server.console('api routes')
  assert.match(all, /POST +\/api\/admin\/posts +admin/)
  assert.match(all, /GET +\/api\/drive +drive/)
  const posts = server.console('api routes /api/admin/notes')
  assert.match(posts, /PATCH +\/api\/admin\/notes\/\{id\}/)
  assert.doesNotMatch(posts, /\/api\/drive/)
})

test('a post from items and a markdown file, as the admin', () => {
  const body = '# 你好\n\n控制台发的 "文章"，有空格和引号。\n'
  writeFileSync(join(files, 'hello.md'), body)
  const created = server.console(
    `api post /api/admin/posts slug=from-console originalLang=zh status=published tags:='[]' ` +
    `translations.zh.title="控制台 你好" translations.zh.body=@${join(files, 'hello.md')}`)
  assert.match(created, /POST \/api\/admin\/posts as author: 201/)
  assert.equal(printedJson(created).slug, 'from-console')

  const read = server.console('api get /api/posts/from-console?lang=zh')
  assert.match(read, /: 200/)
  const post = printedJson(read)
  assert.equal(post.title, '控制台 你好')
  // Saved trimmed, as from the editor
  assert.equal(post.bodyMd ?? post.body, body.trim())
})

test('JSON as it is, validation errors printed, unknown accounts and paths', () => {
  const invalid = server.console('api post /api/admin/notes {"originalLang": "zh", "translations": {"zh": {"title": ""}}}')
  assert.match(invalid, /: 400/)
  assert.match(invalid, /"translations\.zh\.title": "required"/)
  assert.match(server.console('api get /api/nothing'), /: 404/)
  assert.match(server.console('api get /api/admin/posts --as nobody'), /There is no account nobody/)
  assert.match(server.console('api post /api/admin/notes {not json'), /The body isn't JSON/)
  assert.match(server.console('api post /api/admin/notes title'), /an item is key=text/)
  assert.match(server.console('api get admin/posts'), /Syntax: api get/)
})

test('--as another account: its own rights, not the admin\'s', async () => {
  const client = new Client()
  assert.equal((await client.post('/api/auth/register', { username: 'reader', displayName: '读者', email: 'reader@example.com', password: 'password reader' })).status, 201)
  assert.equal((await client.post('/api/auth/verify-email', { token: lastMailTo(server.outbox, 'reader@example.com').token })).status, 200)

  assert.match(server.console('api get /api/admin/site --as reader'), /as reader: 403/)
  const me = server.console('api patch /api/auth/me --as reader@example.com displayName=新名字')
  assert.match(me, /as reader: 200/)
  assert.equal(printedJson(server.console('api get /api/auth/me --as reader')).user.displayName, '新名字')
})

test('the console\'s session is gone after each call', () => {
  server.console('api get /api/auth/me', 'api get /api/admin/posts')
  const { sessions } = printedJson(server.console('api get /api/admin/users/1'))
  // Only the one of the call that lists them
  const console = sessions.filter((s) => s.userAgent === 'blog console')
  assert.equal(console.length, 1)
  assert.equal(console[0].current, true)
})

test('a multipart upload with --form, a download with --out', () => {
  // The smallest PNG: 1×1
  const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64')
  writeFileSync(join(files, 'dot.png'), png)
  const uploaded = server.console(`api post /api/admin/uploads --form file@${join(files, 'dot.png')} width=1 height=1`)
  assert.match(uploaded, /: 201|: 200/)
  const { url } = printedJson(uploaded)
  assert.ok(url)

  const saved = server.console(`api get ${new URL(url, 'http://x').pathname} --out ${join(files, 'back.png')}`)
  assert.match(saved, /saved to/)
  assert.deepEqual(readFileSync(join(files, 'back.png')), png)
})
