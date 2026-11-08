// The admin managing accounts: making, changing and deleting them, roles and what an account may do,
// its logins, and who is online (pnpm test:api)

import assert from 'node:assert/strict'
import { after, before, describe, test } from 'node:test'
import { adminPassword, Client, lastMailTo, origin, startServer, totp, wsUrl } from './server.mjs'

let server
let admin
let adminId
let postId

before(async () => {
  server = await startServer()
  admin = new Client()
  assert.equal((await admin.post('/api/auth/login', { login: 'author', password: adminPassword, totp: totp(server.adminTotpSecret) })).status, 200)
  adminId = (await admin.get('/api/auth/me')).json.user.id
  postId = (await admin.post('/api/admin/posts', {
    slug: 'hello',
    status: 'published',
    originalLang: 'en',
    translations: { en: { title: 'Hello', body: 'Text' } },
  })).json.id
})

after(async () => {
  if (process.exitCode && server)
    console.log(server.log.join(''))
  await server?.stop()
})

// An account made by the admin, logged in
async function made(name, fields = {}) {
  const created = await admin.post('/api/admin/users', {
    username: name, displayName: `${name} 名`, email: `${name}@example.com`, password: `password ${name}`, emailVerified: true, ...fields,
  })
  assert.equal(created.status, 201, JSON.stringify(created.json))
  const client = new Client()
  assert.equal((await client.post('/api/auth/login', { login: name, password: `password ${name}` })).status, 200)
  return { id: created.json.id, client }
}

const comment = (client, body = 'hi') => client.post('/api/comments', { postId, body })

describe('making accounts', () => {
  test('with a password, as a user or moderator, never as admin', async () => {
    const { client } = await made('mona', { role: 'moderator' })
    assert.equal((await client.get('/api/auth/me')).json.user.role, 'moderator')

    const asAdmin = await admin.post('/api/admin/users', { username: 'boss', displayName: 'Boss', email: 'boss@example.com', password: 'password boss', role: 'admin' })
    assert.equal(asAdmin.json.error.fields.role, 'invalid')

    const taken = await admin.post('/api/admin/users', { username: 'MONA', displayName: 'x', email: 'mona@example.com' })
    assert.equal(taken.status, 409)
    assert.deepEqual(taken.json.error.fields, { username: 'taken', email: 'taken' })
  })

  test('without a password: a mail to choose one', async () => {
    const created = await admin.post('/api/admin/users', { username: 'ivy', displayName: 'Ivy', email: 'ivy@example.com', locale: 'en' })
    assert.equal(created.status, 201)
    const mail = lastMailTo(server.outbox, 'ivy@example.com')
    assert.match(mail.text, /your username is ivy/)
    const ivy = new Client()
    assert.equal((await ivy.post('/api/auth/reset-password', { token: mail.token, password: 'password ivy' })).status, 200)
    assert.equal((await ivy.post('/api/auth/login', { login: 'ivy', password: 'password ivy' })).json.user.emailVerified, true)
  })

  test('only the admin', async () => {
    const reader = new Client()
    assert.equal((await reader.post('/api/admin/users', { username: 'x' })).status, 401)
    const { client } = await made('mod2', { role: 'moderator' })
    assert.equal((await client.post('/api/admin/users', { username: 'xyz', displayName: 'x', email: 'x@example.com' })).status, 403)
  })
})

describe('moderators', () => {
  test('hide comments and ban users, not moderators, and see no addresses', async () => {
    const { client: mod } = await made('max', { role: 'moderator' })
    const { id: userId, client: user } = await made('una')
    const { id: otherModId } = await made('moe', { role: 'moderator' })

    const posted = await comment(user)
    assert.equal(posted.status, 201)
    assert.equal((await mod.patch(`/api/admin/comments/${posted.json.id}`, { status: 'hidden' })).json.status, 'hidden')
    assert.equal((await mod.get('/api/admin/comments')).status, 200)

    const list = await mod.get('/api/admin/users?q=una')
    assert.equal(list.json.users[0].email, undefined)
    assert.equal((await admin.get('/api/admin/users?q=una')).json.users[0].email, 'una@example.com')

    assert.equal((await mod.post(`/api/admin/users/${userId}/ban`, { days: 1 })).status, 200)
    assert.equal((await mod.delete(`/api/admin/users/${userId}/ban`)).status, 200)
    assert.equal((await mod.post(`/api/admin/users/${otherModId}/ban`, { days: 1 })).json.error.code, 'cannot_ban_moderator')
    assert.equal((await mod.post(`/api/admin/users/${adminId}/ban`, { days: 1 })).json.error.code, 'cannot_ban_admin')
    assert.equal((await admin.post(`/api/admin/users/${otherModId}/ban`, { days: 1 })).status, 200)

    // Nothing else of the admin's
    assert.equal((await mod.get('/api/admin/site')).status, 403)
    assert.equal((await mod.get(`/api/admin/users/${userId}`)).status, 403)
    assert.equal((await mod.get('/api/admin/ip-bans')).status, 403)
  })
})

describe('changing accounts', () => {
  test('names, address, role, and what it may do', async () => {
    const { id, client } = await made('carl')
    const changed = await admin.patch(`/api/admin/users/${id}`, { username: 'carlos', displayName: 'Carlos', email: 'carlos@example.com' })
    assert.equal(changed.status, 200)
    assert.equal(changed.json.username, 'carlos')
    // A new address isn't verified unless the admin says so
    assert.equal(changed.json.emailVerified, false)
    assert.equal((await admin.patch(`/api/admin/users/${id}`, { emailVerified: true })).json.emailVerified, true)
    // The cached login sees it at once
    assert.equal((await client.get('/api/auth/me')).json.user.username, 'carlos')

    assert.equal((await admin.patch(`/api/admin/users/${id}`, { username: 'author' })).json.error.fields.username, 'taken')
    assert.equal((await admin.patch(`/api/admin/users/${id}`, { denied: ['comment', 'fly'] })).json.error.fields.denied, 'invalid')

    assert.deepEqual((await admin.patch(`/api/admin/users/${id}`, { denied: ['comment', 'react'] })).json.denied, ['comment', 'react'])
    assert.deepEqual((await client.get('/api/auth/me')).json.user.denied, ['comment', 'react'])
    assert.equal((await comment(client)).json.error.code, 'not_allowed')
    assert.equal((await client.post('/api/reactions', { target: `post:${postId}`, kind: 'like', on: true })).json.error.code, 'not_allowed')
    assert.equal((await admin.patch(`/api/admin/users/${id}`, { denied: [] })).status, 200)
    assert.equal((await comment(client)).status, 201)

    assert.equal((await admin.patch(`/api/admin/users/${id}`, { role: 'moderator' })).json.role, 'moderator')
    assert.equal((await client.get('/api/admin/comments')).status, 200)
  })

  test('not the admin accounts', async () => {
    assert.equal((await admin.patch(`/api/admin/users/${adminId}`, { displayName: 'x' })).json.error.code, 'admin_account')
    assert.equal((await admin.delete(`/api/admin/users/${adminId}`)).json.error.code, 'admin_account')
    assert.equal((await admin.delete(`/api/admin/users/${adminId}/sessions`)).json.error.code, 'admin_account')
  })

  test('a new password, a reset link, two-step login', async () => {
    const { id, client } = await made('pat')
    assert.equal((await admin.patch(`/api/admin/users/${id}`, { password: 'short' })).json.error.fields.password, 'too_short')
    assert.equal((await admin.patch(`/api/admin/users/${id}`, { password: 'a new password' })).status, 200)
    // Logged out everywhere
    assert.equal((await client.get('/api/auth/me')).json.user, null)
    assert.equal((await client.post('/api/auth/login', { login: 'pat', password: 'a new password' })).status, 200)

    assert.equal((await admin.post(`/api/admin/users/${id}/password-reset`)).status, 200)
    assert.ok(lastMailTo(server.outbox, 'pat@example.com').text.includes('reset-password?token='))

    const setup = await client.post('/api/auth/totp/setup')
    assert.equal((await client.post('/api/auth/totp/enable', { code: totp(setup.json.secret) })).status, 200)
    assert.equal((await admin.get(`/api/admin/users/${id}`)).json.totpEnabled, true)
    assert.equal((await admin.delete(`/api/admin/users/${id}/totp`)).status, 200)
    assert.equal((await admin.get(`/api/admin/users/${id}`)).json.totpEnabled, false)
    assert.equal((await new Client().post('/api/auth/login', { login: 'pat', password: 'a new password' })).status, 200)
  })

  test('its login sessions, one or all', async () => {
    const { id, client: first } = await made('sam')
    const second = new Client()
    await second.post('/api/auth/login', { login: 'sam', password: 'password sam' })

    const details = (await admin.get(`/api/admin/users/${id}`)).json
    assert.equal(details.sessions.length, 2)
    assert.ok(details.sessions[0].ip)

    // Which one is first's: ending it logs first out, not second
    for (const session of details.sessions) {
      assert.equal((await admin.delete(`/api/admin/users/${id}/sessions/${session.id}`)).status, 200)
      const firstIn = (await first.get('/api/auth/me')).json.user !== null
      const secondIn = (await second.get('/api/auth/me')).json.user !== null
      assert.ok(firstIn !== secondIn || (!firstIn && !secondIn))
    }
    assert.equal((await admin.get(`/api/admin/users/${id}`)).json.sessions.length, 0)

    await first.post('/api/auth/login', { login: 'sam', password: 'password sam' })
    assert.equal((await admin.delete(`/api/admin/users/${id}/sessions`)).status, 200)
    assert.equal((await first.get('/api/auth/me')).json.user, null)
  })
})

describe('deleting accounts', () => {
  test('keeping the comments', async () => {
    const { id, client } = await made('dora')
    const posted = await comment(client, 'still here')
    assert.equal((await admin.delete(`/api/admin/users/${id}?mode=nope`)).status, 400)
    assert.equal((await admin.delete(`/api/admin/users/${id}`)).status, 200)

    const threads = (await new Client().get(`/api/comments?postId=${postId}`)).json.threads
    const kept = threads.find((t) => t.id === posted.json.id)
    assert.equal(kept.body, 'still here')
    assert.equal(kept.author.deleted, true)
    assert.equal(kept.author.displayName, '')

    assert.equal((await client.get('/api/auth/me')).json.user, null)
    assert.equal((await new Client().post('/api/auth/login', { login: 'dora', password: 'password dora' })).status, 401)
    assert.equal((await new Client().post('/api/auth/login', { login: `deleted_${id}`, password: '' })).status, 400)
    // Out of the list, unless asked for
    assert.equal((await admin.get('/api/admin/users?q=dora')).json.users.length, 0)
    const deleted = (await admin.get('/api/admin/users?status=deleted')).json.users
    assert.ok(deleted.some((u) => u.id === id && u.deleted && u.email === `deleted-${id}@invalid`))
    // The name and address are free again
    assert.equal((await made('dora')).client instanceof Client, true)
  })

  test('with everything it wrote', async () => {
    const { id, client } = await made('pete')
    const posted = await comment(client, 'gone soon')
    assert.equal((await admin.delete(`/api/admin/users/${id}?mode=purge`)).status, 200)
    const threads = (await new Client().get(`/api/comments?postId=${postId}`)).json.threads
    assert.equal(threads.find((t) => t.id === posted.json.id), undefined)
    assert.equal((await admin.get(`/api/admin/users/${id}`)).status, 404)
  })
})

describe('online', () => {
  // A page of client's open: its WebSocket, with the client's cookie
  async function openPage(client, path) {
    const cookie = [...client.cookies].map(([k, v]) => `${k}=${v}`).join('; ')
    const socket = new WebSocket(wsUrl, { headers: { Origin: origin, ...(cookie ? { Cookie: cookie } : {}) } })
    await new Promise((resolve, reject) => {
      socket.addEventListener('open', resolve)
      socket.addEventListener('error', reject)
    })
    socket.send(JSON.stringify({ type: 'page', path }))
    return socket
  }

  const wait = (ms) => new Promise((r) => setTimeout(r, ms))
  const find = async (id, status = '') => (await admin.get(`/api/admin/users?status=${status}`)).json.users.find((u) => u.id === id)

  test('who, since when, on which pages, for how long in all', async () => {
    const { id, client } = await made('olga')
    assert.equal((await find(id)).online, null)
    assert.equal((await find(id)).lastSeenAt, null)

    const anonymous = await openPage(new Client(), '/en/posts')
    const page = await openPage(client, '/en/posts/hello')
    // Presence refreshes once a second
    await wait(1500)
    const online = await find(id, 'online')
    assert.ok(online.online.since)
    assert.deepEqual(online.online.pages, ['/en/posts/hello'])
    assert.equal((await admin.get('/api/admin/users?status=online')).json.users.length, 1)
    const counts = (await admin.get('/api/admin/online')).json
    assert.equal(counts.users, 1)
    assert.equal(counts.viewers, 2)

    // Long enough for 2 s counted, at a refresh a second and whole seconds
    await wait(2500)
    page.close()
    anonymous.close()
    await wait(1500)
    const offline = await find(id)
    assert.equal(offline.online, null)
    assert.ok(offline.onlineSeconds >= 2, `online ${offline.onlineSeconds} s`)
    assert.ok(offline.lastSeenAt)
    assert.equal((await admin.get('/api/admin/users?status=online')).json.users.length, 0)
    assert.equal((await admin.get('/api/admin/users?sort=lastSeen')).json.users[0].id, id)
  })

  test('a logged out page is nobody', async () => {
    const { id, client } = await made('lou')
    const socket = await openPage(client, '/en')
    await client.post('/api/auth/logout')
    socket.send(JSON.stringify({ type: 'page', path: '/en/about' }))
    await wait(1500)
    assert.equal((await find(id)).online, null)
    socket.close()
  })
})
