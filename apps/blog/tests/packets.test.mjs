// The API's operations as packets on /ws (ApiPackets.h), every opcode of schema/Admin.fbs and
// schema/Reader.fbs, checked against the HTTP API (pnpm test:api). The packets are made and read by
// flatc, not by the server's code: FLATC names it if it isn't on the PATH.

import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { after, before, describe, test } from 'node:test'
import { adminPassword, Client, lastMailTo, origin, startServer, totp, wsUrl } from './server.mjs'

const flatc = process.env.FLATC ?? 'flatc'
const schemaFile = (name) => fileURLToPath(new URL(`../server/schema/${name}`, import.meta.url))
// Every table of both, Admin's and Reader's
const apiSchema = schemaFile('Api.fbs')
const errorSchema = fileURLToPath(new URL('../../../src/server/shared/packet/Packets.fbs', import.meta.url))

// The opcodes, from the schemas: one place says what they are
const opcodesOf = (file) => Object.fromEntries([...readFileSync(schemaFile(file), 'utf8').matchAll(/^ {2}(\w+) = (0x[0-9A-F]+),$/gm)]
  .map(([, name, value]) => [name, Number(value)]))
const opcodes = opcodesOf('Admin.fbs')
const readerOpcodes = opcodesOf('Reader.fbs')

let server
let admin
let files

before(async () => {
  server = await startServer()
  admin = new Client()
  assert.equal((await admin.post('/api/auth/login', { login: 'author', password: adminPassword, totp: totp(server.adminTotpSecret) })).status, 200)
  files = mkdtempSync(join(tmpdir(), 'blog-packets-'))
})

after(async () => {
  if (process.exitCode && server)
    console.log(server.log.join(''))
  await server?.stop()
  rmSync(files, { recursive: true, force: true })
})

let fileCount = 0

// JSON as a FlatBuffer of type, by flatc
function encode(type, json) {
  const name = `in${++fileCount}`
  writeFileSync(join(files, `${name}.json`), JSON.stringify(json))
  execFileSync(flatc, ['--no-warnings', '-b', '--root-type', type, '-o', files, apiSchema, join(files, `${name}.json`)])
  return readFileSync(join(files, `${name}.bin`))
}

// A FlatBuffer of type as JSON, by flatc
function decode(schema, type, payload) {
  const name = `out${++fileCount}`
  writeFileSync(join(files, `${name}.bin`), payload)
  execFileSync(flatc, ['--no-warnings', '--json', '--strict-json', '--natural-utf8', '--raw-binary', '--root-type', type, '-o', files, schema, '--',
    join(files, `${name}.bin`)])
  return JSON.parse(readFileSync(join(files, `${name}.json`), 'utf8'))
}

// [uint16 opcode][uint16 flags][uint32 requestId][payload], little-endian
function frame(opcode, requestId, payload) {
  const header = Buffer.alloc(8)
  header.writeUInt16LE(opcode, 0)
  header.writeUInt32LE(requestId, 4)
  return Buffer.concat([header, payload])
}

// A WebSocket of a client of ours, logged in as client (its cookie) or not at all
async function connect(client) {
  const headers = { Origin: origin }
  if (client?.cookies.size)
    headers.Cookie = [...client.cookies].map(([k, v]) => `${k}=${v}`).join('; ')
  const socket = new WebSocket(wsUrl, { headers })
  socket.binaryType = 'arraybuffer'
  const waiting = new Map()
  let nextId = 1
  const closed = new Promise((resolve) => socket.addEventListener('close', (event) => resolve(event.code)))
  socket.addEventListener('message', (event) => {
    if (typeof event.data === 'string')
      return
    const data = Buffer.from(event.data)
    const opcode = data.readUInt16LE(0)
    const requestId = data.readUInt32LE(4)
    waiting.get(requestId)?.({ opcode, payload: data.subarray(8) })
    waiting.delete(requestId)
  })
  await new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve)
    socket.addEventListener('error', reject)
  })

  return {
    closed,
    close: () => socket.close(),
    sendRaw: (data) => socket.send(data),
    // The answer to a packet of opcode: its opcode and payload
    exchange(opcode, payload) {
      const requestId = nextId++
      const answer = new Promise((resolve, reject) => {
        waiting.set(requestId, resolve)
        setTimeout(() => reject(new Error(`no answer to opcode ${opcode}`)), 5000)
      })
      socket.send(frame(opcode, requestId, payload))
      return answer
    },
    // The answer to an operation of Blog.<space>: { ok: true, json } or { ok: false, error }
    async send(space, codes, name, request, requestType, responseType) {
      assert.ok(codes[name], `no opcode ${name} in ${space}`)
      const { opcode, payload } = await this.exchange(codes[name], encode(`Blog.${space}.${requestType}`, request))
      if (opcode === 0)
        return { ok: false, error: decode(errorSchema, 'Rooftree.Packets.Error', payload) }
      assert.equal(opcode, codes[name])
      return { ok: true, json: decode(apiSchema, `Blog.${space}.${responseType}`, payload) }
    },
    // The admin's operation's answer
    call(name, request, requestType, responseType) {
      return this.send('Admin', opcodes, name, request, requestType, responseType)
    },
    // A reader's
    read(name, request, requestType, responseType) {
      return this.send('Reader', readerOpcodes, name, request, requestType, responseType)
    },
  }
}

// What HTTP said and what came back in a packet are the same, but that a table doesn't hold a null
// and flatc leaves out a scalar at its default
function assertSame(http, packet, path = '') {
  if (http === null || http === undefined)
    return assert.equal(packet, undefined, `${path}: ${JSON.stringify(packet)} in the packet, ${http} over HTTP`)
  if (packet === undefined)
    return assert.ok(http === 0 || http === false, `${path}: missing from the packet, ${JSON.stringify(http)} over HTTP`)
  if (Array.isArray(http)) {
    assert.ok(Array.isArray(packet), `${path}: not an array in the packet`)
    assert.equal(packet.length, http.length, `${path}: length`)
    return http.forEach((value, i) => assertSame(value, packet[i], `${path}.${i}`))
  }
  if (typeof http === 'object') {
    for (const key of new Set([...Object.keys(http), ...Object.keys(packet)]))
      assertSame(http[key], packet[key], path ? `${path}.${key}` : key)
    return
  }
  if (typeof http === 'number' && !Number.isInteger(http))
    return assert.ok(Math.abs(http - packet) < 1e-6 * Math.max(1, Math.abs(http)), `${path}: ${packet} != ${http}`)
  assert.equal(packet, http, path)
}

async function reader(name, fields = {}) {
  const created = await admin.post('/api/admin/users', {
    username: name, displayName: `${name} 名`, email: `${name}@example.com`, password: `password ${name}`, emailVerified: true, ...fields,
  })
  assert.equal(created.status, 201, JSON.stringify(created.json))
  const client = new Client()
  assert.equal((await client.post('/api/auth/login', { login: name, password: `password ${name}` })).status, 200)
  return { id: created.json.id, client }
}

let ws

describe('admin packets', () => {
  before(async () => { ws = await connect(admin) })
  after(() => ws.close())

  test('posts: create, read, update, delete', async () => {
    assert.equal((await admin.post('/api/admin/tags', { slug: 'packets', nameZh: '包', nameEn: 'Packets' })).status, 201)
    const created = await ws.call('PostsCreate', {
      slug: 'by-packet', status: 'draft', originalLang: 'zh', tags: ['packets'], commentsEnabled: false,
      translations: { zh: { title: '包发的', summary: '摘要', body: '正文' }, en: { title: 'By packet', body: 'Body' } },
    }, 'PostInput', 'Post')
    assert.ok(created.ok, JSON.stringify(created.error))
    const id = created.json.id
    assertSame((await admin.get(`/api/admin/posts/${id}`)).json, created.json)
    // A scalar at its default isn't in the buffer, flatc leaves it out: false
    assert.equal(created.json.commentsEnabled, undefined)

    const got = await ws.call('PostsGet', { id }, 'IdRequest', 'Post')
    assertSame((await admin.get(`/api/admin/posts/${id}`)).json, got.json)

    // Scheduled, then not: "" is null; an empty language is deleted
    const at = new Date(Date.now() + 86400000).toISOString()
    const scheduled = await ws.call('PostsUpdate', { id, publishAt: at, translations: { en: {} } }, 'PostInput', 'Post')
    assert.ok(scheduled.ok, JSON.stringify(scheduled.error))
    assert.equal(scheduled.json.publishAt.slice(0, 16), at.slice(0, 16))
    assert.deepEqual(Object.keys(scheduled.json.translations), ['zh'])
    const unscheduled = await ws.call('PostsUpdate', { id, publishAt: '', status: 'published' }, 'PostInput', 'Post')
    assert.equal(unscheduled.json.publishAt, undefined)
    assert.equal(unscheduled.json.status, 'published')
    assertSame((await admin.get(`/api/admin/posts/${id}`)).json, unscheduled.json)

    const list = await ws.call('PostsList', { status: 'published' }, 'PostsListRequest', 'PostsList')
    assertSame((await admin.get('/api/admin/posts?status=published')).json, list.json)
    assert.ok(list.json.posts.some((post) => post.slug === 'by-packet'))

    assert.deepEqual((await ws.call('PostsDelete', { id }, 'IdRequest', 'Ok')).json, { ok: true })
    assert.equal((await admin.get(`/api/admin/posts/${id}`)).status, 404)
  })

  test('errors: validation, conflict, not found, nothing that fits', async () => {
    const invalid = await ws.call('PostsCreate', { slug: 'x', originalLang: 'fr', translations: {} }, 'PostInput', 'Post')
    assert.equal(invalid.ok, false)
    assert.equal(invalid.error.status, 400)
    assert.equal(invalid.error.code, 'validation_failed')
    assert.equal(invalid.error.opcode, opcodes.PostsCreate)
    assert.ok(invalid.error.fields.some((field) => field.name === 'originalLang' && field.code === 'invalid'))

    const missing = await ws.call('PostsGet', { id: 999999 }, 'IdRequest', 'Post')
    assert.deepEqual([missing.error.status, missing.error.code], [404, 'not_found'])
    const noId = await ws.call('NotesGet', {}, 'IdRequest', 'Note')
    assert.equal(noId.error.status, 404)

    assert.equal((await ws.call('TagsCreate', { slug: 'twice', nameZh: '两', nameEn: 'Two' }, 'TagInput', 'Tag')).json.postCount, undefined)
    const taken = await ws.call('TagsCreate', { slug: 'twice', nameZh: '两', nameEn: 'Two' }, 'TagInput', 'Tag')
    assert.deepEqual([taken.error.status, taken.error.code, taken.error.fields], [409, 'conflict', [{ name: 'slug', code: 'taken' }]])
  })

  test('tags', async () => {
    const created = await ws.call('TagsCreate', { slug: 'net', nameZh: '网', nameEn: 'Net' }, 'TagInput', 'Tag')
    assert.deepEqual(created.json, { slug: 'net', nameZh: '网', nameEn: 'Net' })
    const updated = await ws.call('TagsUpdate', { slug: 'net', nameEn: 'Network' }, 'TagInput', 'Tag')
    assert.deepEqual(updated.json, { slug: 'net', nameZh: '网', nameEn: 'Network' })
    assert.ok((await new Client().get('/api/tags')).json.tags.some((tag) => tag.slug === 'net'))
    assert.deepEqual((await ws.call('TagsDelete', { slug: 'net' }, 'SlugRequest', 'Ok')).json, { ok: true })
  })

  test('notes and thinking', async () => {
    const note = await ws.call('NotesCreate', {
      status: 'draft', originalLang: 'zh', mood: '好', weather: '晴', translations: { zh: { title: '今天', body: '看星星' } },
    }, 'NoteInput', 'Note')
    assert.ok(note.ok, JSON.stringify(note.error))
    const id = note.json.id
    assertSame((await admin.get(`/api/admin/notes/${id}`)).json, note.json)
    const edited = await ws.call('NotesUpdate', { id, mood: '累', translations: { en: { title: 'Today', body: 'Stars' } } }, 'NoteInput', 'Note')
    assertSame((await admin.get(`/api/admin/notes/${id}`)).json, edited.json)
    assertSame((await admin.get(`/api/admin/notes/${id}`)).json, (await ws.call('NotesGet', { id }, 'IdRequest', 'Note')).json)
    assertSame((await admin.get('/api/admin/notes?page=1')).json, (await ws.call('NotesList', { page: 1 }, 'NotesListRequest', 'NotesList')).json)
    assert.deepEqual((await ws.call('NotesDelete', { id }, 'IdRequest', 'Ok')).json, { ok: true })

    const thinking = await ws.call('ThinkingCreate', { originalLang: 'zh', translations: { zh: { body: '人间有味是清欢' } } }, 'ThinkingInput', 'Thinking')
    const thinkingId = thinking.json.id
    assert.equal(thinking.json.status, 'published')
    const unpublished = await ws.call('ThinkingUpdate', { id: thinkingId, status: 'draft' }, 'ThinkingInput', 'Thinking')
    assertSame((await admin.get(`/api/admin/thinking/${thinkingId}`)).json, unpublished.json)
    assertSame((await admin.get(`/api/admin/thinking/${thinkingId}`)).json,
      (await ws.call('ThinkingGet', { id: thinkingId }, 'IdRequest', 'Thinking')).json)
    assertSame((await admin.get('/api/admin/thinking')).json, (await ws.call('ThinkingList', {}, 'PageRequest', 'ThinkingList')).json)
    assert.deepEqual((await ws.call('ThinkingDelete', { id: thinkingId }, 'IdRequest', 'Ok')).json, { ok: true })
  })

  test('comments', async () => {
    const post = await admin.post('/api/admin/posts', { slug: 'commented-by-packet', status: 'published', originalLang: 'zh', translations: { zh: { title: 't', body: 'b' } } })
    const { client: alice } = await reader('alice')
    const comment = (await alice.post('/api/comments', { postId: post.json.id, body: '看得到吗' })).json
    const list = await ws.call('CommentsList', {}, 'CommentsListRequest', 'CommentsList')
    assertSame((await admin.get('/api/admin/comments')).json, list.json)
    const hidden = await ws.call('CommentsModerate', { id: comment.id, status: 'hidden' }, 'CommentsModerateRequest', 'Comment')
    assert.equal(hidden.json.status, 'hidden')
    assert.equal(hidden.json.author.username, 'alice')
    assertSame((await admin.get('/api/admin/comments?status=hidden')).json,
      (await ws.call('CommentsList', { status: 'hidden' }, 'CommentsListRequest', 'CommentsList')).json)
  })

  test('friends and projects', async () => {
    const friend = await ws.call('FriendsCreate', { name: '邻居', url: 'https://neighbour.example.com', description: '隔壁', sort: 0 }, 'FriendInput', 'Friend')
    assert.ok(friend.ok, JSON.stringify(friend.error))
    const edited = await ws.call('FriendsUpdate', { id: friend.json.id, name: '邻居', url: 'https://neighbour.example.com', sort: -3 }, 'FriendInput', 'Friend')
    assert.equal(edited.json.sort, -3)
    const list = await ws.call('FriendsList', {}, 'FriendsListRequest', 'FriendsList')
    assertSame((await admin.get('/api/admin/friends')).json, list.json)
    assert.deepEqual((await ws.call('FriendsDelete', { id: friend.json.id }, 'IdRequest', 'Ok')).json, { ok: true })

    const project = await ws.call('ProjectsCreate', { name: 'Rooftree', url: 'https://example.com/tc', descriptionZh: '框架' }, 'ProjectInput', 'Project')
    assert.ok(project.ok, JSON.stringify(project.error))
    const renamed = await ws.call('ProjectsUpdate', { id: project.json.id, name: 'rooftree', url: 'https://example.com/tc' }, 'ProjectInput', 'Project')
    assertSame((await new Client().get('/api/projects')).json.projects.find((p) => p.id === project.json.id), renamed.json)
    assert.deepEqual((await ws.call('ProjectsDelete', { id: project.json.id }, 'IdRequest', 'Ok')).json, { ok: true })
  })

  test('the site', async () => {
    const site = await ws.call('SiteGet', {}, 'EmptyRequest', 'SiteProfile')
    assertSame((await admin.get('/api/admin/site')).json, site.json)
    const updated = await ws.call('SiteUpdate', {
      ...site.json, author: '包', tagline: { zh: '标语', en: 'Tagline' }, socials: [{ kind: 'github', url: 'https://github.com/example' }],
    }, 'SiteProfile', 'SiteProfile')
    assert.ok(updated.ok, JSON.stringify(updated.error))
    assertSame((await admin.get('/api/admin/site')).json, updated.json)
    assert.equal(updated.json.tagline.en, 'Tagline')
  })

  test('accounts', async () => {
    const created = await ws.call('UsersCreate', { username: 'pat', displayName: '帕特', email: 'pat@example.com', password: 'password pat' }, 'UserInput', 'UserCreated')
    assert.ok(created.ok, JSON.stringify(created.error))
    const id = created.json.id
    const pat = new Client()
    assert.equal((await pat.post('/api/auth/login', { login: 'pat', password: 'password pat' })).status, 200)

    const got = await ws.call('UsersGet', { id }, 'IdRequest', 'User')
    assertSame({ ...(await admin.get(`/api/admin/users/${id}`)).json, lastSeenAt: got.json.lastSeenAt }, got.json)
    assert.equal(got.json.sessions.length, 1)

    const updated = await ws.call('UsersUpdate', { id, displayName: '帕特二', denied: ['comment'] }, 'UserInput', 'User')
    assert.equal(updated.json.displayName, '帕特二')
    assert.deepEqual(updated.json.denied, ['comment'])
    const drive = await ws.call('UsersSetDrive', { id, enabled: true, quota: 5 * 1024 ** 3, trashDays: 3 }, 'UsersSetDriveRequest', 'User')
    assert.deepEqual([drive.json.drive.enabled, drive.json.drive.quota, drive.json.drive.trashDays], [true, 5 * 1024 ** 3, 3])

    const list = await ws.call('UsersList', { q: 'pat' }, 'UsersListRequest', 'UsersList')
    const httpList = (await admin.get('/api/admin/users?q=pat')).json
    assertSame({ ...httpList, users: httpList.users.map((user, i) => ({ ...user, lastSeenAt: list.json.users[i]?.lastSeenAt })) }, list.json)

    const banned = await ws.call('UsersBan', { id, reason: '吵', days: 2 }, 'UsersBanRequest', 'UsersBanned')
    assert.equal(banned.json.ok, true)
    assert.ok(banned.json.bannedUntil)
    assert.deepEqual((await ws.call('UsersUnban', { id }, 'IdRequest', 'Ok')).json, { ok: true })

    assert.deepEqual((await ws.call('UsersMailPasswordReset', { id }, 'IdRequest', 'Ok')).json, { ok: true })
    assert.ok(lastMailTo(server.outbox, 'pat@example.com').token)
    const noTotp = await ws.call('UsersDisableTotp', { id }, 'IdRequest', 'Ok')
    assert.ok(noTotp.ok || noTotp.error.status === 409, JSON.stringify(noTotp))

    const session = (await ws.call('UsersGet', { id }, 'IdRequest', 'User')).json.sessions[0].id
    assert.deepEqual((await ws.call('UsersDeleteSession', { id, session }, 'UsersDeleteSessionRequest', 'Ok')).json, { ok: true })
    assert.equal((await pat.get('/api/auth/me')).json.user, null)
    assert.deepEqual((await ws.call('UsersDeleteSessions', { id }, 'IdRequest', 'Ok')).json, { ok: true })
    assert.deepEqual((await ws.call('UsersDelete', { id, mode: 'anonymize' }, 'UsersDeleteRequest', 'Ok')).json, { ok: true })
  })

  test('banned addresses', async () => {
    const ban = await ws.call('IpBansAdd', { ip: '203.0.113.9', reason: '刷屏', days: 1 }, 'IpBansAddRequest', 'IpBan')
    assert.ok(ban.ok, JSON.stringify(ban.error))
    assertSame((await admin.get('/api/admin/ip-bans')).json, (await ws.call('IpBansList', {}, 'EmptyRequest', 'IpBansList')).json)
    assert.deepEqual((await ws.call('IpBansDelete', { ip: '203.0.113.9' }, 'IpBanRequest', 'Ok')).json, { ok: true })
  })

  test('chat moderation: the square and the groups', async () => {
    const { client: chatter } = await reader('apchatter')
    const sent = (await chatter.post('/api/chat/channels/square/messages', { content: '要被删的' })).json
    await chatter.post('/api/chat/channels/square/messages', { content: '留着的' })
    const list = await ws.call('ChatMessagesList', { channel: 'square' }, 'ChatMessagesListRequest', 'ChatMessagesList')
    assert.ok(list.ok, JSON.stringify(list.error))
    assertSame((await admin.get('/api/admin/chat/messages?channel=square')).json, list.json)
    assert.equal(list.json.messages[0].content, '留着的')
    assert.deepEqual((await ws.call('ChatMessageDelete', { id: sent.id }, 'ChatMessageRequest', 'ChatRemoved')).json, { removed: 1 })
    assert.equal((await ws.call('ChatMessageDelete', { id: sent.id }, 'ChatMessageRequest', 'ChatRemoved')).error.status, 404)
    // Never a direct chat
    const direct = await ws.call('ChatMessagesList', { channel: 'dm:1:2' }, 'ChatMessagesListRequest', 'ChatMessagesList')
    assert.deepEqual(direct.error.fields, [{ name: 'channel', code: 'invalid' }])

    const group = (await chatter.post('/api/chat/groups', { name: '包里的群', memberIds: [] })).json
    await chatter.post(`/api/chat/channels/${encodeURIComponent(group.id)}/messages`, { content: '群里说的' })
    const groups = await ws.call('ChatGroupsList', {}, 'EmptyRequest', 'ChatGroupsList')
    assertSame((await admin.get('/api/admin/chat/groups')).json, groups.json)
    assert.deepEqual(groups.json.groups.map((g) => [g.name, g.members, g.messages]), [['包里的群', 1, 1]])
    assert.deepEqual((await ws.call('ChatChannelClear', { channel: group.id }, 'ChatChannelRequest', 'ChatRemoved')).json, { removed: 1 })
    const groupId = Number(group.id.slice('group:'.length))
    // Nothing left in it: flatc leaves out the 0
    assert.deepEqual((await ws.call('ChatGroupDelete', { id: groupId }, 'IdRequest', 'ChatRemoved')).json, {})
    assert.equal((await ws.call('ChatGroupDelete', { id: groupId }, 'IdRequest', 'ChatRemoved')).error.status, 404)
    assert.ok((await ws.call('ChatChannelClear', { channel: 'square' }, 'ChatChannelRequest', 'ChatRemoved')).json.removed >= 1)
  })

  test('an upload', async () => {
    const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64')
    const uploaded = await ws.call('Upload', { name: 'dot.png', data: [...png], width: 1, height: 1, thumbhash: 'AAAA' }, 'UploadRequest', 'Uploaded')
    assert.ok(uploaded.ok, JSON.stringify(uploaded.error))
    assert.deepEqual([uploaded.json.mime, uploaded.json.size], ['image/png', png.length])
    const response = await fetch(`${wsUrl.replace('ws://', 'http://').replace('/ws', '')}${uploaded.json.url}`)
    assert.deepEqual(Buffer.from(await response.arrayBuffer()), png)

    const notImage = await ws.call('Upload', { name: 'x.png', data: [1, 2, 3] }, 'UploadRequest', 'Uploaded')
    assert.deepEqual(notImage.error.fields, [{ name: 'file', code: 'unsupported_type' }])
  })

  test('the numbers', async () => {
    const online = await ws.call('Online', {}, 'EmptyRequest', 'OnlineCounts')
    assert.ok(online.ok)
    assertSame((await admin.get('/api/admin/stats?days=3')).json, (await ws.call('Stats', { days: 3 }, 'StatsRequest', 'Stats')).json)
    // The disk's free bytes change from one call to the next
    const drives = (await ws.call('Drives', {}, 'EmptyRequest', 'Drives')).json
    const httpDrives = (await admin.get('/api/admin/drives')).json
    assertSame({ ...httpDrives, disk: { ...httpDrives.disk, free: drives.disk.free } }, drives)
    const status = await ws.call('Status', { since: 0 }, 'StatusRequest', 'Status')
    assert.ok(status.json.now.cores >= 1)
    assert.ok(status.json.interval > 0)
  })
})

// An Error's details by name: a value or a list
const details = (error) => Object.fromEntries((error.details ?? []).map((detail) => [detail.name, detail.values ?? detail.value]))

describe('reader packets', () => {
  let anonymous
  let alice
  let asAlice
  before(async () => {
    anonymous = await connect()
    alice = (await reader('rpalice')).client
    asAlice = await connect(alice)
  })
  after(() => {
    anonymous.close()
    asAlice.close()
  })

  test('posts and tags, by anyone', async () => {
    assert.equal((await admin.post('/api/admin/tags', { slug: 'leaf', nameZh: '叶', nameEn: 'Leaf' })).status, 201)
    const made = (await admin.post('/api/admin/posts', { slug: 'read-by-packet', status: 'published', originalLang: 'zh', tags: ['leaf'],
      translations: { zh: { title: '包读的', summary: '摘', body: '正文' }, en: { title: 'Read by packet', body: 'Body' } } })).json
    await admin.post('/api/admin/posts', { slug: 'english-only', status: 'published', originalLang: 'en', translations: { en: { title: 'E', body: 'e' } } })
    const anon = new Client()

    for (const query of [{}, { lang: 'en' }, { tag: 'leaf', limit: 1 }]) {
      const list = await anonymous.read('PostsList', query, 'PostsListRequest', 'PostsList')
      assert.ok(list.ok, JSON.stringify(list.error))
      assertSame((await anon.get(`/api/posts?${new URLSearchParams(query)}`)).json, list.json)
    }
    const first = await anonymous.read('PostsList', { limit: 1 }, 'PostsListRequest', 'PostsList')
    const next = await anonymous.read('PostsList', { limit: 1, cursor: first.json.nextCursor }, 'PostsListRequest', 'PostsList')
    assertSame((await anon.get(`/api/posts?limit=1&cursor=${first.json.nextCursor}`)).json, next.json)
    assert.equal((await anonymous.read('PostsList', { cursor: 'nope!' }, 'PostsListRequest', 'PostsList')).error.code, 'invalid_cursor')

    const post = await anonymous.read('PostsGet', { slug: 'read-by-packet', lang: 'en' }, 'PostsGetRequest', 'Post')
    assertSame((await anon.get('/api/posts/read-by-packet?lang=en')).json, post.json)
    assertSame((await alice.get('/api/posts/read-by-packet')).json, (await asAlice.read('PostsGet', { slug: 'read-by-packet' }, 'PostsGetRequest', 'Post')).json)
    const notInLang = await anonymous.read('PostsGet', { slug: 'english-only' }, 'PostsGetRequest', 'Post')
    assert.deepEqual([notInLang.error.status, notInLang.error.code, details(notInLang.error)], [404, 'not_in_lang', { translations: ['en'] }])
    await admin.patch(`/api/admin/posts/${made.id}`, { slug: 'read-by-packet-2' })
    const moved = await anonymous.read('PostsGet', { slug: 'read-by-packet', lang: 'en' }, 'PostsGetRequest', 'Post')
    assert.deepEqual([moved.error.status, moved.error.code, details(moved.error)],
      [301, 'moved', { redirect: 'read-by-packet-2', location: '/api/posts/read-by-packet-2?lang=en' }])

    assertSame((await anon.get('/api/posts/slugs')).json, (await anonymous.read('PostsSlugs', {}, 'EmptyRequest', 'PostsSlugs')).json)
    assertSame((await anon.get('/api/tags?lang=en')).json, (await anonymous.read('TagsList', { lang: 'en' }, 'TagsListRequest', 'TagsList')).json)
    assertSame((await anon.get('/api/tags')).json, (await anonymous.read('TagsList', {}, 'TagsListRequest', 'TagsList')).json)
    assertSame((await anon.get('/api/posts/search?q=Read&all=1')).json,
      (await anonymous.read('PostsSearch', { q: 'Read', all: '1' }, 'PostsSearchRequest', 'PostsSearch')).json)
    const empty = await anonymous.read('PostsSearch', { q: '' }, 'PostsSearchRequest', 'PostsSearch')
    assert.deepEqual([empty.error.code, empty.error.fields], ['validation_failed', [{ name: 'q', code: 'too_short' }]])
  })

  test('notes, thinking and the site, by anyone', async () => {
    const note = (await admin.post('/api/admin/notes', { status: 'published', originalLang: 'zh', mood: '好',
      translations: { zh: { title: '包', body: '读' }, en: { title: 'Packet', body: 'Read' } } })).json
    await admin.post('/api/admin/thinking', { originalLang: 'zh', translations: { zh: { body: '包里的想法' } } })
    const anon = new Client()
    assertSame((await anon.get('/api/notes?lang=en')).json, (await anonymous.read('NotesList', { lang: 'en' }, 'PageRequest', 'NotesList')).json)
    assertSame((await anon.get(`/api/notes/${note.id}?lang=en`)).json,
      (await anonymous.read('NotesGet', { id: note.id, lang: 'en' }, 'NotesGetRequest', 'Note')).json)
    assert.equal((await anonymous.read('NotesGet', {}, 'NotesGetRequest', 'Note')).error.status, 404)
    assertSame((await anon.get('/api/thinking?limit=2')).json, (await anonymous.read('ThinkingList', { limit: 2 }, 'PageRequest', 'ThinkingList')).json)

    assertSame((await anon.get('/api/site?lang=en')).json, (await anonymous.read('SiteGet', { lang: 'en' }, 'LangRequest', 'Site')).json)
    assertSame((await anon.get('/api/site/about')).json, (await anonymous.read('SiteAbout', {}, 'LangRequest', 'About')).json)
    assertSame((await anon.get('/api/timeline')).json, (await anonymous.read('Timeline', {}, 'LangRequest', 'Timeline')).json)
    assertSame((await anon.get('/api/friends')).json, (await anonymous.read('FriendsList', {}, 'EmptyRequest', 'FriendsList')).json)
    assertSame((await anon.get('/api/projects')).json, (await anonymous.read('ProjectsList', {}, 'EmptyRequest', 'ProjectsList')).json)
    // An object by address (json_map): flatc shows the entries as they are, with their key
    const meta = (await anonymous.read('UploadsMeta', {}, 'EmptyRequest', 'UploadsMeta')).json
    const httpMeta = (await anon.get('/api/uploads/meta')).json
    assertSame(httpMeta.images, Object.fromEntries((meta.images ?? []).map(({ key, ...image }) => [key, image])))
  })

  test('comments, reactions and views', async () => {
    const post = (await admin.post('/api/admin/posts', { slug: 'talked-by-packet', status: 'published', originalLang: 'zh',
      translations: { zh: { title: '聊', body: '天' } } })).json
    const target = `post:${post.id}`
    const made = await asAlice.read('CommentsCreate', { target, body: '包里的评论' }, 'CommentInput', 'Comment')
    assert.ok(made.ok, JSON.stringify(made.error))
    const reply = await asAlice.read('CommentsCreate', { target, body: '回复', replyTo: made.json.id }, 'CommentInput', 'Comment')
    assert.equal(reply.json.parentId, made.json.id)
    const edited = await asAlice.read('CommentsUpdate', { id: made.json.id, body: '改过' }, 'CommentsUpdateRequest', 'Comment')
    assert.equal(edited.json.body, '改过')
    const anon = new Client()
    const threads = await anonymous.read('CommentsList', { target }, 'CommentsListRequest', 'CommentsList')
    assertSame((await anon.get(`/api/comments?target=${target}`)).json, threads.json)
    assert.equal(threads.json.threads[0].replies.length, 1)
    const invalid = await asAlice.read('CommentsCreate', { target: 'nope', body: '' }, 'CommentInput', 'Comment')
    assert.deepEqual(invalid.error.fields, [{ name: 'target', code: 'required' }, { name: 'body', code: 'required' }])
    assert.equal((await anonymous.read('CommentsCreate', { target, body: 'x' }, 'CommentInput', 'Comment')).error.status, 401)

    const liked = await asAlice.read('React', { target, kind: 'like', on: true }, 'ReactRequest', 'Reactions')
    assert.deepEqual(liked.json.mine, ['like'])
    // Objects by key (json_map): flatc shows the entries as they are, with their key
    const reactions = (await asAlice.read('ReactionsList', { targets: target }, 'ReactionsListRequest', 'ReactionsList')).json.reactions
    assertSame((await alice.get(`/api/reactions?targets=${target}`)).json.reactions, Object.fromEntries(reactions.map(({ key, counts, mine }) =>
      [key, { counts: Object.fromEntries(counts.map((count) => [count.key, count.count])), mine }])))
    assert.deepEqual((await anonymous.read('CountView', { path: '/zh' }, 'ViewRequest', 'Ok')).json, { ok: true })

    // A banned account may still take its comments down, nothing else
    const { id: bannedId, client: banned } = await reader('rpbanned')
    const asBanned = await connect(banned)
    const own = await asBanned.read('CommentsCreate', { target, body: '要被封了' }, 'CommentInput', 'Comment')
    assert.equal((await admin.post(`/api/admin/users/${bannedId}/ban`, { reason: '吵', days: 1 })).status, 200)
    const refused = await asBanned.read('CommentsCreate', { target, body: 'x' }, 'CommentInput', 'Comment')
    assert.deepEqual([refused.error.status, refused.error.code, details(refused.error).reason], [403, 'banned', '吵'])
    assert.ok(details(refused.error).until)
    assert.deepEqual((await asBanned.read('CommentsDelete', { id: own.json.id }, 'IdRequest', 'Ok')).json, { ok: true })
    asBanned.close()
  })

  test('the account: who, its profile, picture, two-step login, mails', async () => {
    assert.deepEqual((await anonymous.read('Me', {}, 'EmptyRequest', 'Me')).json, {})
    const me = await asAlice.read('Me', {}, 'EmptyRequest', 'Me')
    assertSame((await alice.get('/api/auth/me')).json, me.json)
    assert.equal(me.json.csrfToken, alice.csrfToken)
    assertSame((await new Client().get('/api/captcha')).json, (await anonymous.read('Captcha', {}, 'EmptyRequest', 'Captcha')).json)
    assertSame((await new Client().get('/api/auth/providers')).json, (await anonymous.read('OAuthProviders', {}, 'EmptyRequest', 'OAuthProviders')).json)

    const updated = await asAlice.read('MeUpdate', { displayName: '包里的名字', replyMails: false }, 'MeUpdateRequest', 'Profile')
    assert.equal(updated.json.user.displayName, '包里的名字')
    assertSame({ user: (await alice.get('/api/auth/me')).json.user }, updated.json)
    const invalid = await asAlice.read('MeUpdate', { displayName: '', locale: 'fr' }, 'MeUpdateRequest', 'Profile')
    assert.deepEqual(invalid.error.fields, [{ name: 'displayName', code: 'required' }, { name: 'locale', code: 'invalid' }])
    assert.equal((await anonymous.read('MeUpdate', { displayName: 'x' }, 'MeUpdateRequest', 'Profile')).error.status, 401)

    const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64')
    const pictured = await asAlice.read('AvatarUpload', { file: [...png] }, 'AvatarUploadRequest', 'Profile')
    assert.ok(pictured.json.user.avatar.startsWith('/uploads/'))
    assert.deepEqual((await asAlice.read('AvatarUpload', {}, 'AvatarUploadRequest', 'Profile')).error.fields, [{ name: 'file', code: 'required' }])
    assert.equal((await asAlice.read('AvatarDelete', {}, 'EmptyRequest', 'Profile')).json.user.avatar, undefined)

    const totp = await asAlice.read('TotpSetup', {}, 'EmptyRequest', 'TotpSecret')
    assert.match(totp.json.otpauthUri, /^otpauth:\/\/totp\//)
    const wrong = await asAlice.read('TotpEnable', { code: '000000' }, 'TotpCodeRequest', 'Ok')
    assert.deepEqual(wrong.error.fields, [{ name: 'code', code: 'incorrect' }])
    assertSame((await alice.get('/api/auth/identities')).json, (await asAlice.read('IdentitiesList', {}, 'EmptyRequest', 'IdentitiesList')).json)

    assert.deepEqual((await asAlice.read('SubscriptionSet', { subscribed: true }, 'SubscriptionSetRequest', 'Subscription')).json, { subscribed: true })
    assert.deepEqual((await asAlice.read('SubscriptionGet', {}, 'EmptyRequest', 'Subscription')).json, { subscribed: true })
    assert.deepEqual((await anonymous.read('Subscribe', { email: 'by-packet@example.com' }, 'SubscribeRequest', 'Ok')).json, { ok: true })
    assert.ok(lastMailTo(server.outbox, 'by-packet@example.com').token)
    assert.deepEqual((await anonymous.read('ForgotPassword', { email: 'rpalice@example.com' }, 'EmailRequest', 'Ok')).json, { ok: true })
  })

  test('the drive and its links', async () => {
    const { id: daveId, client: dave } = await reader('rpdave')
    const asDave = await connect(dave)
    assert.equal((await asDave.read('DriveList', {}, 'DriveListRequest', 'DriveList')).error.code, 'no_drive')
    await admin.put(`/api/admin/users/${daveId}/drive`, { enabled: true, quota: 100000, trashDays: 1 })

    const folder = await asDave.read('DriveFolderCreate', { name: '包' }, 'DriveFolderInput', 'DriveFolder')
    assert.ok(folder.ok, JSON.stringify(folder.error))
    const inner = await asDave.read('DriveFolderCreate', { parentId: folder.json.id, name: '里面' }, 'DriveFolderInput', 'DriveFolder')
    // null is the root (json_null): 0 in the packet
    const moved = await asDave.read('DriveFolderUpdate', { id: inner.json.id, parentId: 0 }, 'DriveFolderInput', 'DriveFolder')
    assert.equal(moved.json.parentId, undefined)
    assertSame((await dave.get('/api/drive/folders')).json, (await asDave.read('DriveFolders', {}, 'DriveRequest', 'DriveFolders')).json)

    // The pieces go over HTTP, the rest in packets
    const upload = await asDave.read('DriveUploadCreate', { folderId: folder.json.id, name: 'a.txt', size: 3 }, 'DriveUploadInput', 'DriveUpload')
    assert.ok(upload.ok, JSON.stringify(upload.error))
    assert.equal((await dave.binary('PUT', `/api/drive/uploads/${upload.json.id}?offset=0`, Buffer.from('abc'))).status, 200)
    const file = await asDave.read('DriveUploadComplete', { id: upload.json.id }, 'DriveUploadRequest', 'DriveFile')
    assert.deepEqual([file.json.name, file.json.size], ['a.txt', 3])
    const list = await asDave.read('DriveList', { folder: folder.json.id }, 'DriveListRequest', 'DriveList')
    assertSame((await dave.get(`/api/drive?folder=${folder.json.id}`)).json, list.json)

    const share = await asDave.read('DriveShareCreate', { folderId: folder.json.id, expires: '1d', password: '包' }, 'DriveShareInput', 'DriveShare')
    assert.ok(share.ok, JSON.stringify(share.error))
    assertSame((await dave.get('/api/drive/shares')).json, (await asDave.read('DriveSharesList', {}, 'DriveRequest', 'DriveShares')).json)
    const locked = await asAlice.read('ShareGet', { token: share.json.token }, 'ShareGetRequest', 'ShareView')
    assert.equal(locked.json.share.locked, true)
    assert.equal(locked.json.files, undefined)
    assert.equal((await asAlice.read('ShareUnlock', { token: share.json.token, password: '错' }, 'ShareUnlockRequest', 'Ok')).error.code, 'wrong_password')
    assert.deepEqual((await asAlice.read('ShareUnlock', { token: share.json.token, password: '包' }, 'ShareUnlockRequest', 'Ok')).json, { ok: true })
    const opened = await asAlice.read('ShareGet', { token: share.json.token }, 'ShareGetRequest', 'ShareView')
    assertSame((await alice.get(`/api/shares/${share.json.token}`)).json, opened.json)
    assert.equal((await anonymous.read('ShareGet', { token: share.json.token }, 'ShareGetRequest', 'ShareView')).error.status, 401)

    const deleted = await asDave.read('DriveFileDelete', { id: file.json.id }, 'DriveIdRequest', 'DriveDeleted')
    assert.deepEqual(deleted.json, { ok: true, trashed: true })
    assertSame((await dave.get('/api/drive/trash')).json, (await asDave.read('DriveTrash', {}, 'DriveRequest', 'DriveTrash')).json)
    const restored = await asDave.read('DriveRestore', { kind: 'file', id: file.json.id }, 'DriveTrashRequest', 'DriveRestored')
    assert.deepEqual([restored.json.kind, restored.json.name, restored.json.folderId], ['file', 'a.txt', folder.json.id])

    // The admin, on another account's drive
    const asAdmin = await connect(admin)
    assertSame((await admin.get(`/api/drive?user=${daveId}`)).json, (await asAdmin.read('DriveList', { user: daveId }, 'DriveListRequest', 'DriveList')).json)
    assert.equal((await asAlice.read('DriveList', { user: daveId }, 'DriveListRequest', 'DriveList')).error.code, 'forbidden')
    asAdmin.close()
    asDave.close()
  })

  test('the password and logging out: the connection calls as the new session, then as nobody', async () => {
    const { client } = await reader('rppass')
    const conn = await connect(client)
    try {
      const wrong = await conn.read('PasswordChange', { currentPassword: 'not it', newPassword: 'a new password' }, 'PasswordChangeRequest', 'Me')
      assert.equal(wrong.ok, false)
      assert.equal(wrong.error.code, 'validation_failed')

      // Every session of the account ends, the connection's goes on as the new one
      const changed = await conn.read('PasswordChange', { currentPassword: 'password rppass', newPassword: 'a new password' }, 'PasswordChangeRequest', 'Me')
      assert.ok(changed.ok, JSON.stringify(changed.error))
      assert.equal(changed.json.user.username, 'rppass')
      assert.equal((await conn.read('Me', {}, 'EmptyRequest', 'Me')).json.user.username, 'rppass')
      // The page's cookie was one of the sessions that ended
      assert.equal((await client.get('/api/auth/me')).json.user, null)
      assert.equal((await new Client().post('/api/auth/login', { login: 'rppass', password: 'a new password' })).status, 200)

      assert.ok((await conn.read('Logout', {}, 'EmptyRequest', 'Ok')).ok)
      assert.equal((await conn.read('Me', {}, 'EmptyRequest', 'Me')).json.user, undefined)
      const after = await conn.read('Logout', {}, 'EmptyRequest', 'Ok')
      assert.equal(after.error.code, 'unauthenticated')
    } finally {
      conn.close()
    }
  })

  test('chat: the square, a group, read and unread', async () => {
    const { id: carlId, client: carl } = await reader('rpcarl')
    await alice.post('/api/user/friends/requests', { to: 'rpcarl' })
    const incoming = (await carl.get('/api/user/friends/requests')).json.incoming
    assert.equal((await carl.post(`/api/user/friends/requests/${incoming[0].id}/accept`)).status, 200)

    const sent = await asAlice.read('ChatMessageSend', { channelId: 'square', content: '  包里的聊天  ' }, 'ChatMessageSendRequest', 'ChatMessageItem')
    assert.ok(sent.ok, JSON.stringify(sent.error))
    assert.equal(sent.json.content, '包里的聊天')
    assertSame((await alice.get('/api/chat/channels')).json, (await asAlice.read('ChatChannelsList', {}, 'EmptyRequest', 'ChatChannelsList')).json)
    assertSame((await alice.get('/api/chat/channels/square/messages')).json,
      (await asAlice.read('ChatMessagesList', { channelId: 'square' }, 'ChatMessagesListRequest', 'ChatMessagesList')).json)
    assert.deepEqual((await asAlice.read('ChatMarkRead', { channelId: 'square', lastReadId: sent.json.id }, 'ChatMarkReadRequest', 'Ok')).json, { ok: true })
    assertSame((await alice.get('/api/chat/unread')).json, (await asAlice.read('ChatUnreadCount', {}, 'EmptyRequest', 'ChatUnreadCount')).json)
    const tooLong = await asAlice.read('ChatMessageSend', { channelId: 'square', content: '长'.repeat(1001) }, 'ChatMessageSendRequest', 'ChatMessageItem')
    assert.deepEqual(tooLong.error.fields, [{ name: 'content', code: 'too_long' }])
    assert.equal((await anonymous.read('ChatUnreadCount', {}, 'EmptyRequest', 'ChatUnreadCount')).error.status, 401)

    const group = await asAlice.read('ChatGroupCreate', { name: '包里的群', memberIds: [carlId] }, 'ChatGroupCreateRequest', 'ChatChannelItem')
    assert.ok(group.ok, JSON.stringify(group.error))
    assert.equal(group.json.memberCount, 2)
    const groupId = Number(group.json.id.slice('group:'.length))
    assertSame((await alice.get(`/api/chat/groups/${groupId}/members`)).json,
      (await asAlice.read('ChatGroupMembers', { id: groupId }, 'ChatGroupActionRequest', 'ChatGroupMembersList')).json)
    assert.deepEqual((await asAlice.read('ChatGroupLeave', { id: groupId }, 'ChatGroupActionRequest', 'Ok')).json, { ok: true })
    assert.equal((await carl.get(`/api/chat/groups/${groupId}/members`)).status, 403)
  })

  test('a friend link: a login asks, anyone may not', async () => {
    const anonymousApplies = await anonymous.read('FriendsApply', { name: 'x', url: 'https://x.example.com' }, 'FriendApplication', 'FriendApplied')
    assert.equal(anonymousApplies.error.status, 401)
    const applied = await asAlice.read('FriendsApply', { name: 'Alice', url: 'https://alice.example.com', message: '你好' }, 'FriendApplication', 'FriendApplied')
    assert.ok(applied.ok, JSON.stringify(applied.error))
    assert.equal(applied.json.status, 'pending')
    assert.ok((await admin.get('/api/admin/friends?status=pending')).json.friends.some((link) => link.id === applied.json.id))
  })
})

describe('who may send them', () => {
  test('nobody logged in, a reader, a moderator', async () => {
    const anonymous = await connect()
    assert.equal((await anonymous.call('PostsList', {}, 'PostsListRequest', 'PostsList')).error.status, 401)
    anonymous.close()

    const { client: bob } = await reader('bob')
    const asReader = await connect(bob)
    assert.equal((await asReader.call('SiteGet', {}, 'EmptyRequest', 'SiteProfile')).error.code, 'forbidden')
    asReader.close()

    const { client: mod } = await reader('mod', { role: 'moderator' })
    const asModerator = await connect(mod)
    assert.ok((await asModerator.call('CommentsList', {}, 'CommentsListRequest', 'CommentsList')).ok)
    assert.ok((await asModerator.call('Online', {}, 'EmptyRequest', 'OnlineCounts')).ok)
    assert.equal((await asModerator.call('SiteGet', {}, 'EmptyRequest', 'SiteProfile')).error.code, 'forbidden')
    // Logged out since the handshake
    await mod.post('/api/auth/logout')
    assert.equal((await asModerator.call('Online', {}, 'EmptyRequest', 'OnlineCounts')).error.status, 401)
    asModerator.close()
  })

  test('an unknown opcode, a packet that is not one', async () => {
    const socket = await connect(admin)
    const { opcode, payload } = await socket.exchange(0x7FFF, encode('Blog.Admin.EmptyRequest', {}))
    assert.equal(opcode, 0)
    assert.deepEqual(decode(errorSchema, 'Rooftree.Packets.Error', payload), { status: 400, code: 'unknown_opcode', opcode: 0x7FFF })
    socket.sendRaw(frame(opcodes.PostsList, 1, Buffer.from([1, 2, 3])))
    assert.equal(await socket.closed, 1007)
  })
})
