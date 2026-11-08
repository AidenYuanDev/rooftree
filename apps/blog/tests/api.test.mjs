// The blog API against a real server and database: pnpm test:api in apps/blog (see server.mjs for
// what it needs). Each describe() builds on the state of the ones before it.

import assert from 'node:assert/strict'
import { after, before, describe, test } from 'node:test'
import { adminPassword, baseUrl, Client, lastMailTo, startServer, totp } from './server.mjs'

let server

before(async () => { server = await startServer() })
after(async () => {
  if (process.exitCode && server)
    console.log(server.log.join(''))
  await server?.stop()
})

async function register(client, username, extra = {}) {
  const email = `${username}@example.com`
  const response = await client.post('/api/auth/register', { username, displayName: username, email, password: 'password for ' + username, ...extra })
  assert.equal(response.status, 201, JSON.stringify(response.json))
  return { email, password: 'password for ' + username }
}

async function registerAndLogin(username, extra) {
  const client = new Client()
  const { email, password } = await register(client, username, extra)
  const verify = await client.post('/api/auth/verify-email', { token: lastMailTo(server.outbox, email).token })
  assert.equal(verify.status, 200)
  const login = await client.post('/api/auth/login', { login: username, password })
  assert.equal(login.status, 200, JSON.stringify(login.json))
  return client
}

let adminClient
async function loginAdmin(client = new Client(), offset = 0) {
  const response = await client.post('/api/auth/login', { login: 'author', password: adminPassword, totp: totp(server.adminTotpSecret, offset) })
  assert.equal(response.status, 200, JSON.stringify(response.json))
  return client
}

describe('accounts', () => {
  test('me without a login', async () => {
    const response = await new Client().get('/api/auth/me')
    assert.deepEqual(response.json, { user: null, csrfToken: null })
  })

  test('register checks the origin and every field', async () => {
    const response = await fetch(`${baseUrl}/api/auth/register`, { method: 'POST', body: '{}', headers: { 'Content-Type': 'application/json' } })
    assert.equal(response.status, 403)
    assert.equal((await response.json()).error.code, 'origin_denied')

    const invalid = await new Client().post('/api/auth/register', { username: 'a b', displayName: '', email: 'nope', password: 'short', locale: 'fr' })
    assert.equal(invalid.status, 400)
    assert.deepEqual(invalid.json.error, {
      code: 'validation_failed',
      fields: { username: 'invalid', displayName: 'required', email: 'invalid', password: 'too_short', locale: 'invalid' },
    })

    assert.equal((await new Client().post('/api/auth/register', 'not json')).json.error.code, 'invalid_json')
  })

  test('register, verify by mail, log in, log out', async () => {
    const client = new Client()
    const { email, password } = await register(client, 'alice', { displayName: '爱丽丝', locale: 'zh' })

    const mail = lastMailTo(server.outbox, email)
    assert.match(mail.text, /爱丽丝，你好/)
    assert.match(mail.text, /http:\/\/localhost:5173\/zh\/verify-email\?token=/)

    const taken = await client.post('/api/auth/register', { username: 'ALICE', displayName: 'x', email, password: 'whatever123' })
    assert.equal(taken.status, 409)
    assert.deepEqual(taken.json.error.fields, { username: 'taken', email: 'taken' })

    assert.equal((await client.post('/api/auth/verify-email', { token: mail.token })).status, 200)
    assert.equal((await client.post('/api/auth/verify-email', { token: mail.token })).json.error.code, 'invalid_token')

    assert.equal((await client.post('/api/auth/login', { login: 'alice', password: 'wrong password' })).json.error.code, 'invalid_credentials')
    assert.equal((await client.post('/api/auth/login', { login: 'nobody', password: 'wrong password' })).json.error.code, 'invalid_credentials')

    const login = await client.post('/api/auth/login', { login: email.toUpperCase(), password })
    assert.equal(login.status, 200)
    assert.equal(login.json.user.displayName, '爱丽丝')
    assert.equal(login.json.user.emailVerified, true)
    assert.equal(login.json.user.email, email)
    assert.equal(login.json.csrfToken.length, 43)
    assert.equal(client.cookies.get('blog_session').length, 43)

    assert.equal((await client.get('/api/auth/me')).json.user.username, 'alice')

    assert.equal((await client.post('/api/auth/logout')).status, 200)
    assert.equal(client.cookies.size, 0)
    assert.equal((await client.get('/api/auth/me')).json.user, null)
  })

  test('state-changing requests need the CSRF token and our origin', async () => {
    const client = await registerAndLogin('bob')
    const token = client.csrfToken

    client.csrfToken = null
    assert.equal((await client.patch('/api/auth/me', { displayName: 'Bobby' })).json.error.code, 'csrf_failed')
    client.csrfToken = 'x'.repeat(43)
    assert.equal((await client.patch('/api/auth/me', { displayName: 'Bobby' })).json.error.code, 'csrf_failed')
    client.csrfToken = token
    assert.equal((await client.patch('/api/auth/me', { displayName: 'Bobby' }, { Origin: 'https://evil.example.com' })).json.error.code, 'csrf_failed')

    const updated = await client.patch('/api/auth/me', { displayName: 'Bobby', locale: 'en' })
    assert.equal(updated.status, 200)
    assert.equal(updated.json.user.displayName, 'Bobby')
    assert.equal((await client.get('/api/auth/me')).json.user.locale, 'en')

    // Reads need neither
    client.csrfToken = null
    assert.equal((await client.get('/api/auth/me', { Origin: 'https://evil.example.com' })).json.user.username, 'bob')
  })

  test('a made-up or expired cookie is no login', async () => {
    const client = new Client()
    client.cookies.set('blog_session', 'A'.repeat(43))
    assert.equal((await client.get('/api/auth/me')).json.user, null)
    assert.equal((await client.post('/api/auth/logout')).status, 401)
  })

  test('password reset by mail ends every session', async () => {
    const client = await registerAndLogin('carol')
    const other = new Client()
    assert.equal((await other.post('/api/auth/login', { login: 'carol', password: 'password for carol' })).status, 200)

    // Any address gets the same answer
    assert.equal((await new Client().post('/api/auth/forgot-password', { email: 'nobody@example.com' })).status, 200)
    assert.equal(lastMailTo(server.outbox, 'nobody@example.com'), undefined)

    assert.equal((await new Client().post('/api/auth/forgot-password', { email: 'carol@example.com' })).status, 200)
    const mail = lastMailTo(server.outbox, 'carol@example.com')
    assert.match(mail.text, /reset-password\?token=/)

    const reset = await new Client().post('/api/auth/reset-password', { token: mail.token, password: 'a new password' })
    assert.equal(reset.status, 200)
    assert.equal((await new Client().post('/api/auth/reset-password', { token: mail.token, password: 'a new password' })).json.error.code, 'invalid_token')

    assert.equal((await client.get('/api/auth/me')).json.user, null)
    assert.equal((await other.get('/api/auth/me')).json.user, null)
    assert.equal((await new Client().post('/api/auth/login', { login: 'carol', password: 'password for carol' })).status, 401)
    assert.equal((await new Client().post('/api/auth/login', { login: 'carol', password: 'a new password' })).status, 200)
  })

  test('changing the password keeps only this session', async () => {
    const client = await registerAndLogin('dave')
    const other = new Client()
    await other.post('/api/auth/login', { login: 'dave', password: 'password for dave' })

    const wrong = await client.post('/api/auth/change-password', { currentPassword: 'nope', newPassword: 'another password' })
    assert.deepEqual(wrong.json.error.fields, { currentPassword: 'incorrect' })

    const changed = await client.post('/api/auth/change-password', { currentPassword: 'password for dave', newPassword: 'another password' })
    assert.equal(changed.status, 200)
    assert.equal((await client.get('/api/auth/me')).json.user.username, 'dave')
    assert.equal((await other.get('/api/auth/me')).json.user, null)
  })

  test('wrong passwords lock the login name for a while', async () => {
    const client = await registerAndLogin('erin')
    for (let i = 0; i < 3; ++i)
      assert.equal((await new Client().post('/api/auth/login', { login: 'erin', password: 'wrong' })).status, 401)

    const locked = await new Client().post('/api/auth/login', { login: 'erin', password: 'password for erin' })
    assert.equal(locked.status, 429)
    assert.equal(locked.json.error.code, 'too_many_attempts')
    assert.ok(Number(locked.headers.get('retry-after')) > 0)

    // Logged in sessions don't care
    assert.equal((await client.get('/api/auth/me')).json.user.username, 'erin')
  })

  test('two-step login for a user', async () => {
    const client = await registerAndLogin('frank')
    const setup = await client.post('/api/auth/totp/setup')
    assert.equal(setup.status, 200)
    assert.match(setup.json.otpauthUri, /^otpauth:\/\/totp\/My%20Blog:frank\?secret=/)

    assert.deepEqual((await client.post('/api/auth/totp/enable', { code: '000000' })).json.error.fields, { code: 'incorrect' })
    assert.equal((await client.post('/api/auth/totp/enable', { code: totp(setup.json.secret) })).status, 200)
    assert.equal((await client.get('/api/auth/me')).json.user.totpEnabled, true)

    const login = new Client()
    assert.equal((await login.post('/api/auth/login', { login: 'frank', password: 'password for frank' })).json.error.code, 'totp_required')
    // The code enabling it was used already, the next one is fine
    const code = totp(setup.json.secret, 1)
    assert.equal((await login.post('/api/auth/login', { login: 'frank', password: 'password for frank', totp: code })).status, 200)
    assert.equal((await login.post('/api/auth/login', { login: 'frank', password: 'password for frank', totp: code })).json.error.code, 'totp_invalid')

    assert.equal((await client.post('/api/auth/totp/disable', { password: 'password for frank' })).status, 200)
    assert.equal((await new Client().post('/api/auth/login', { login: 'frank', password: 'password for frank' })).status, 200)
  })

  test('the admin logs in with its TOTP', async () => {
    const client = new Client()
    assert.equal((await client.post('/api/auth/login', { login: 'author', password: adminPassword })).json.error.code, 'totp_required')
    adminClient = await loginAdmin(client)
    assert.equal((await adminClient.get('/api/auth/me')).json.user.role, 'admin')
  })
})

describe('posts', () => {
  const post = (slug, extra = {}) => ({
    slug,
    status: 'published',
    originalLang: 'zh',
    tags: ['cpp'],
    translations: { zh: { title: `标题 ${slug}`, summary: '摘要', body: `# ${slug}\n\n正文` } },
    ...extra,
  })

  test('only the admin and who it lets write do (tests/editors.test.mjs)', async () => {
    const user = await registerAndLogin('gina')
    assert.equal((await user.post('/api/admin/posts', post('x'))).json.error.code, 'not_allowed')
    assert.equal((await new Client().post('/api/admin/posts', post('x'))).status, 401)
    // A reader's list: the posts it edits, none
    assert.deepEqual((await user.get('/api/admin/posts')).json.posts, [])
  })

  test('tags', async () => {
    assert.equal((await adminClient.post('/api/admin/tags', { slug: 'cpp', nameZh: 'C++', nameEn: 'C++' })).status, 201)
    assert.equal((await adminClient.post('/api/admin/tags', { slug: 'net', nameZh: '网络', nameEn: 'Network' })).status, 201)
    assert.equal((await adminClient.post('/api/admin/tags', { slug: 'cpp', nameZh: 'x', nameEn: 'x' })).status, 409)
    assert.equal((await adminClient.patch('/api/admin/tags/net', { nameEn: 'Networking' })).json.nameEn, 'Networking')
    assert.equal((await adminClient.post('/api/admin/tags', { slug: 'tmp', nameZh: '临时', nameEn: 'Tmp' })).status, 201)
    assert.equal((await adminClient.delete('/api/admin/tags/tmp')).status, 200)
    assert.deepEqual((await new Client().get('/api/tags')).json.tags.map((t) => t.slug), ['cpp', 'net'])
  })

  test('create, validate and read', async () => {
    const invalid = await adminClient.post('/api/admin/posts', { slug: 'Not A Slug', originalLang: 'en', tags: ['nope'], translations: { zh: { title: '' } } })
    assert.equal(invalid.status, 400)
    assert.deepEqual(invalid.json.error.fields, {
      slug: 'invalid', tags: 'unknown_tag', 'translations.zh.title': 'required', 'translations.zh.body': 'required', 'translations.en': 'required',
    })

    const created = await adminClient.post('/api/admin/posts', post('hello', {
      translations: {
        zh: { title: '你好', summary: '第一篇', body: '# 你好' },
        en: { title: 'Hello', summary: 'First', body: '# Hello' },
      },
    }))
    assert.equal(created.status, 201)
    assert.deepEqual(created.json.tags, ['cpp'])
    assert.ok(created.json.publishedAt)
    assert.equal((await adminClient.post('/api/admin/posts', post('hello'))).status, 409)

    const zh = await new Client().get('/api/posts/hello?lang=zh')
    assert.equal(zh.json.title, '你好')
    assert.equal(zh.json.author, 'The Author')
    assert.deepEqual(zh.json.tags, [{ slug: 'cpp', name: 'C++' }])
    // In the order of the column's ENUM
    assert.deepEqual(zh.json.translations, ['zh', 'en'])
    const en = await new Client().get('/api/posts/hello?lang=en')
    assert.equal(en.json.title, 'Hello')
    assert.equal(en.json.fallback, false)

    assert.equal((await new Client().get('/api/posts/missing')).status, 404)
  })

  test('drafts stay private', async () => {
    const draft = await adminClient.post('/api/admin/posts', post('secret-draft', { status: 'draft' }))
    assert.equal(draft.json.publishedAt, null)
    assert.equal((await new Client().get('/api/posts/secret-draft')).status, 404)
    assert.ok(!(await new Client().get('/api/posts')).json.posts.some((p) => p.slug === 'secret-draft'))
    assert.equal((await adminClient.get(`/api/admin/posts/${draft.json.id}`)).json.slug, 'secret-draft')
    assert.ok((await adminClient.get('/api/admin/posts?status=draft')).json.posts.some((p) => p.slug === 'secret-draft'))
  })

  test('a draft is published at its time', async () => {
    const soon = new Date(Date.now() + 2000).toISOString()
    assert.deepEqual((await adminClient.post('/api/admin/posts', post('too-late', { status: 'draft', publishAt: '2001-01-01T00:00:00.000Z' }))).json.error.fields, { publishAt: 'past' })
    assert.deepEqual((await adminClient.post('/api/admin/posts', post('bad-time', { status: 'draft', publishAt: 'tomorrow' }))).json.error.fields, { publishAt: 'invalid' })

    const later = await adminClient.post('/api/admin/posts', post('later', { status: 'draft', publishAt: soon }))
    assert.equal(later.status, 201)
    assert.equal(later.json.status, 'draft')
    assert.equal(Math.abs(Date.parse(later.json.publishAt) - Date.parse(soon)) <= 1000, true)
    assert.equal((await adminClient.get('/api/admin/posts?status=draft')).json.posts.find((p) => p.slug === 'later').publishAt, later.json.publishAt)
    assert.equal((await new Client().get('/api/posts/later')).status, 404)

    // Published right away: no time kept; a draft's time taken back
    const now = await adminClient.post('/api/admin/posts', post('right-away', { publishAt: soon }))
    assert.equal(now.json.publishAt, null)
    const kept = await adminClient.post('/api/admin/posts', post('kept-draft', { status: 'draft', publishAt: new Date(Date.now() + 3600_000).toISOString() }))
    assert.equal((await adminClient.patch(`/api/admin/posts/${kept.json.id}`, { publishAt: null })).json.publishAt, null)

    let published
    for (let i = 0; i < 40 && !published; ++i) {
      await new Promise((resolve) => setTimeout(resolve, 250))
      const read = await new Client().get('/api/posts/later')
      if (read.status === 200) published = read.json
    }
    assert.ok(published, 'published within 10 seconds')
    const admin = (await adminClient.get(`/api/admin/posts/${later.json.id}`)).json
    assert.deepEqual([admin.status, admin.publishAt], ['published', null])
    assert.ok(Date.parse(admin.publishedAt) >= Date.parse(soon) - 1000)
    assert.equal((await adminClient.get(`/api/admin/posts/${kept.json.id}`)).json.status, 'draft')

    // Not in the lists of the tests after
    for (const id of [later.json.id, now.json.id, kept.json.id])
      assert.equal((await adminClient.delete(`/api/admin/posts/${id}`)).status, 200)
  })

  test('lists page by cursor, each language its own posts', async () => {
    for (const slug of ['second', 'third', 'fourth'])
      assert.equal((await adminClient.post('/api/admin/posts', post(slug, { tags: ['net'] }))).status, 201)

    const first = await new Client().get('/api/posts?lang=zh&limit=2')
    assert.deepEqual(first.json.posts.map((p) => p.slug), ['fourth', 'third'])
    assert.equal(first.json.posts[0].fallback, false)
    assert.equal(first.json.posts[0].lang, 'zh')
    assert.deepEqual(first.json.posts[0].tags, [{ slug: 'net', name: '网络' }])
    assert.ok(first.json.nextCursor)

    const second = await new Client().get(`/api/posts?lang=zh&limit=2&cursor=${first.json.nextCursor}`)
    assert.deepEqual(second.json.posts.map((p) => p.slug), ['second', 'hello'])
    assert.equal(second.json.nextCursor, null)

    // The English site has only the post written in English too, and tags count in their language
    assert.deepEqual((await new Client().get('/api/posts?lang=en')).json.posts.map((p) => p.slug), ['hello'])
    const count = async (query) => Object.fromEntries((await new Client().get(`/api/tags${query}`)).json.tags.map((t) => [t.slug, t.postCount]))
    assert.deepEqual(await count('?lang=zh'), { cpp: 1, net: 3 })
    assert.deepEqual(await count('?lang=en'), { cpp: 1, net: 0 })
    assert.deepEqual(await count(''), { cpp: 1, net: 3 })
    const third = await new Client().get('/api/posts/third?lang=en')
    assert.equal(third.status, 404)
    assert.deepEqual(third.json.error, { code: 'not_in_lang', translations: ['zh'] })

    assert.deepEqual((await new Client().get('/api/posts?tag=cpp')).json.posts.map((p) => p.slug), ['hello'])
    assert.equal((await new Client().get('/api/posts?cursor=nope')).json.error.code, 'invalid_cursor')
  })

  test('a changed slug redirects', async () => {
    const id = (await new Client().get('/api/posts/hello')).json.id
    const updated = await adminClient.patch(`/api/admin/posts/${id}`, { slug: 'hello-world', tags: ['cpp', 'net'], translations: { en: null } })
    assert.equal(updated.status, 200)
    assert.deepEqual(Object.keys(updated.json.translations), ['zh'])

    const old = await new Client().get('/api/posts/hello?lang=en')
    assert.equal(old.status, 301)
    assert.equal(old.headers.get('location'), '/api/posts/hello-world?lang=en')
    // For a client that doesn't follow it: an error as a packet's would be, naming the new slug
    assert.deepEqual(old.json.error, { code: 'moved', redirect: 'hello-world', location: '/api/posts/hello-world?lang=en' })

    // fetch follows it by default, to the post not in English anymore
    const followed = await fetch(`${baseUrl}/api/posts/hello?lang=en`)
    assert.equal((await followed.json()).error.code, 'not_in_lang')
    assert.equal((await (await fetch(`${baseUrl}/api/posts/hello?lang=zh`)).json()).slug, 'hello-world')

    const slugs = await new Client().get('/api/posts/slugs')
    assert.deepEqual(slugs.json.redirects, [{ from: 'hello', to: 'hello-world' }])
    assert.ok(slugs.json.posts.some((p) => p.slug === 'hello-world'))
    assert.ok(!slugs.json.posts.some((p) => p.slug === 'secret-draft'))

    const missingOriginal = await adminClient.patch(`/api/admin/posts/${id}`, { translations: { zh: null } })
    assert.deepEqual(missingOriginal.json.error.fields, { 'translations.zh': 'required' })

    // The old slug can be taken by a new post, which ends the redirect
    assert.equal((await adminClient.post('/api/admin/posts', post('hello'))).status, 201)
    assert.equal((await new Client().get('/api/posts/hello')).status, 200)
  })

  test('delete', async () => {
    const id = (await new Client().get('/api/posts/fourth')).json.id
    assert.equal((await adminClient.delete(`/api/admin/posts/${id}`)).status, 200)
    assert.equal((await new Client().get('/api/posts/fourth')).status, 404)
    assert.equal((await adminClient.delete(`/api/admin/posts/${id}`)).status, 404)
  })
})
