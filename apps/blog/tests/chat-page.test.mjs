// The chat page's API (pnpm test:api): replies, files and pictures, edits and recalls, @mentions,
// search, pinned and muted chats, the group owner's management.

import assert from 'node:assert/strict'
import { readdirSync } from 'node:fs'
import { join } from 'node:path'
import { after, before, describe, test } from 'node:test'
import { adminPassword, baseUrl, Client, lastMailTo, origin, startServer, totp, wsUrl } from './server.mjs'

let server

before(async () => {
  server = await startServer()
})

after(async () => {
  if (process.exitCode && server)
    console.log(server.log.join(''))
  await server?.stop()
})

// The smallest PNG: one transparent pixel
const png = Buffer.from(
  '89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d49444154789c6360000002000154a24f5d0000000049454e44ae426082',
  'hex',
)

async function makeUser(name) {
  const client = new Client()
  const email = `${name}@example.com`
  assert.equal((await client.post('/api/auth/register', { username: name, displayName: `${name} Name`, email, password: `password ${name}` })).status, 201)
  assert.equal((await client.post('/api/auth/verify-email', { token: lastMailTo(server.outbox, email).token })).status, 200)
  assert.equal((await client.post('/api/auth/login', { login: name, password: `password ${name}` })).status, 200)
  client.id = (await client.get('/api/auth/me')).json.user.id
  client.name = name
  return client
}

async function befriend(a, b) {
  assert.equal((await a.post('/api/user/friends/requests', { to: b.name })).status, 200)
  const incoming = (await b.get('/api/user/friends/requests')).json.incoming.find((r) => r.user.username === a.name)
  assert.equal((await b.post(`/api/user/friends/requests/${incoming.id}/accept`)).status, 200)
}

const dm = (a, b) => `dm:${Math.min(a.id, b.id)}:${Math.max(a.id, b.id)}`

function chatFiles() {
  try {
    return readdirSync(join(server.uploadsDir, 'chat'))
  } catch {
    return []
  }
}

async function sendFile(client, channelId, bytes, name, fields = {}) {
  const form = new FormData()
  form.append('file', new Blob([bytes]), name)
  for (const [key, value] of Object.entries(fields)) form.append(key, String(value))
  return client.post(`/api/chat/channels/${encodeURIComponent(channelId)}/files`, form)
}

// A page's WebSocket, logged in as client; ready once the server counted it
async function openPage(client) {
  const headers = { Origin: origin, Cookie: [...client.cookies].map(([k, v]) => `${k}=${v}`).join('; ') }
  const socket = new WebSocket(wsUrl, { headers })
  const received = []
  let counted
  const ready = new Promise((resolve) => { counted = resolve })
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data)
    if (message.type === 'presence')
      counted()
    received.push(message)
  })
  await new Promise((resolve) => socket.addEventListener('open', resolve))
  socket.send(JSON.stringify({ type: 'page', path: '/zh/chat', watch: [] }))
  await ready
  return {
    received,
    close: () => socket.close(),
    async waitFor(predicate, ms = 3000) {
      for (const until = Date.now() + ms; Date.now() < until; await new Promise((resolve) => setTimeout(resolve, 20))) {
        const found = received.find(predicate)
        if (found)
          return found
      }
      throw new Error('nothing like it came')
    },
  }
}

describe('the chat page', () => {
  // ann owns the group (ann, bob, cat); dan is ann's friend only; eve is nobody's
  let ann, bob, cat, dan, eve
  let group

  before(async () => {
    ann = await makeUser('pageann')
    bob = await makeUser('pagebob')
    cat = await makeUser('pagecat')
    dan = await makeUser('pagedan')
    eve = await makeUser('pageeve')
    await befriend(ann, bob)
    await befriend(ann, cat)
    await befriend(ann, dan)
    const made = await ann.post('/api/chat/groups', { name: 'Garden', memberIds: [bob.id, cat.id] })
    assert.equal(made.status, 200)
    group = made.json.id
    assert.equal(made.json.ownerId, ann.id)
  })

  test('a reply quotes a message of the same chat only', async () => {
    const first = await bob.post(`/api/chat/channels/${group}/messages`, { content: 'Who waters the roses?' })
    const reply = await ann.post(`/api/chat/channels/${group}/messages`, { content: 'I do', replyToId: first.json.id })
    assert.equal(reply.status, 200)
    assert.deepEqual(reply.json.replyTo, {
      id: first.json.id, senderId: bob.id, senderName: 'pagebob Name', content: 'Who waters the roses?', hasFile: false, recalled: false,
    })
    // As read back, and in the list's last message
    const listed = (await cat.get(`/api/chat/channels/${group}/messages`)).json.messages.at(-1)
    assert.equal(listed.replyTo.id, first.json.id)

    // Not one of another chat, nor one that isn't there
    const square = await bob.post('/api/chat/channels/square/messages', { content: 'square words' })
    assert.equal((await ann.post(`/api/chat/channels/${group}/messages`, { content: 'x', replyToId: square.json.id })).json.error.code, 'invalid_reply')
    assert.equal((await ann.post(`/api/chat/channels/${group}/messages`, { content: 'x', replyToId: 999999 })).status, 400)
  })

  test('a file reaches the chat\'s readers only; a picture is shown, the rest downloaded', async () => {
    const before = chatFiles().length
    const picture = await sendFile(bob, group, png, 'dot.png', { content: 'a dot' })
    assert.equal(picture.status, 200)
    assert.equal(picture.json.content, 'a dot')
    assert.deepEqual({ ...picture.json.file, url: undefined }, { name: 'dot.png', mime: 'image/png', size: png.length, image: true, url: undefined })
    assert.equal(picture.json.file.url, `/api/chat/files/${picture.json.id}`)
    assert.equal(chatFiles().length, before + 1)

    const shown = await cat.binary('GET', picture.json.file.url)
    assert.equal(shown.status, 200)
    assert.equal(shown.headers.get('content-type'), 'image/png')
    assert.ok(shown.data.equals(png))
    assert.match(shown.headers.get('content-disposition'), /^inline/)

    // Not to someone outside the group, nor to nobody
    assert.equal((await eve.binary('GET', picture.json.file.url)).status, 404)
    assert.equal((await new Client().binary('GET', picture.json.file.url)).status, 401)

    // Anything else is downloaded, whatever it is called; a "picture" that isn't one is no picture
    const text = await sendFile(ann, dm(ann, dan), Buffer.from('<script>alert(1)</script>'), '../../evil.html')
    assert.equal(text.status, 200)
    assert.equal(text.json.file.name, 'evil.html')
    assert.equal(text.json.file.image, false)
    const fake = await sendFile(ann, dm(ann, dan), Buffer.from('not a png'), 'fake.png')
    assert.equal(fake.json.file.image, false)
    const downloaded = await dan.binary('GET', text.json.file.url)
    assert.equal(downloaded.headers.get('content-type'), 'application/octet-stream')
    assert.match(downloaded.headers.get('content-disposition'), /^attachment/)
    assert.equal((await bob.binary('GET', text.json.file.url)).status, 404)

    // No file, no message
    const empty = new FormData()
    empty.append('content', 'nothing')
    assert.equal((await ann.post(`/api/chat/channels/${group}/files`, empty)).status, 400)
  })

  test('its sender edits a message for a while, nobody else', async () => {
    // Ann's page: a page open counts its account online a little after it closed, and cat must be away
    // for the mention mail below
    const page = await openPage(ann)
    const sent = await bob.post(`/api/chat/channels/${group}/messages`, { content: 'tomorow' })
    const edited = await bob.patch(`/api/chat/messages/${sent.json.id}`, { content: 'tomorrow' })
    assert.equal(edited.status, 200)
    assert.equal(edited.json.content, 'tomorrow')
    assert.ok(edited.json.editedAt)
    const pushed = await page.waitFor((m) => m.type === 'chat_updated' && m.id === sent.json.id)
    assert.equal(pushed.content, 'tomorrow')
    page.close()

    assert.equal((await ann.patch(`/api/chat/messages/${sent.json.id}`, { content: 'mine now' })).status, 403)
    assert.equal((await eve.patch(`/api/chat/messages/${sent.json.id}`, { content: 'mine now' })).status, 404)
    assert.equal((await bob.patch(`/api/chat/messages/${sent.json.id}`, { content: '  ' })).status, 400)
  })

  test('its sender takes a message back: its text, file and mentions go, a reply shows it went', async () => {
    const files = chatFiles().length
    const sent = await sendFile(bob, group, png, 'gone.png', { content: 'hey @pagecat' })
    assert.deepEqual(sent.json.mentions, [cat.id])
    assert.equal(chatFiles().length, files + 1)
    const reply = await cat.post(`/api/chat/channels/${group}/messages`, { content: 'nice', replyToId: sent.json.id })

    assert.equal((await ann.post(`/api/chat/messages/${sent.json.id}/recall`)).status, 403)
    const recalled = await bob.post(`/api/chat/messages/${sent.json.id}/recall`)
    assert.equal(recalled.status, 200)
    assert.equal(recalled.json.recalled, true)
    assert.equal(recalled.json.content, '')
    assert.equal(recalled.json.file, null)
    assert.deepEqual(recalled.json.mentions, [])
    assert.equal(chatFiles().length, files)
    assert.equal((await cat.binary('GET', `/api/chat/files/${sent.json.id}`)).status, 404)

    const quoted = (await ann.get(`/api/chat/channels/${group}/messages`)).json.messages.find((m) => m.id === reply.json.id)
    assert.equal(quoted.replyTo.recalled, true)
    assert.equal(quoted.replyTo.content, '')

    // Once back, never again, nor edited
    assert.equal((await bob.post(`/api/chat/messages/${sent.json.id}/recall`)).json.error.code, 'recalled')
    assert.equal((await bob.patch(`/api/chat/messages/${sent.json.id}`, { content: 'again' })).status, 409)
  })

  test('a mention counts for the mentioned until read, and mails them when away', async () => {
    // Everything read first
    for (const client of [ann, bob, cat]) {
      const last = (await client.get(`/api/chat/channels/${group}/messages`)).json.messages.at(-1)
      await client.post(`/api/chat/channels/${group}/read`, { lastReadId: last.id })
    }

    // Mails are the reply mails' switch, off until turned on: cat's on, bob's off
    assert.equal((await cat.patch('/api/auth/me', { replyMails: true })).json.user.replyMails, true)
    assert.equal((await dan.patch('/api/auth/me', { replyMails: true })).json.user.replyMails, true)
    const bobMails = lastMailTo(server.outbox, 'pagebob@example.com')

    const sent = await ann.post(`/api/chat/channels/${group}/messages`, { content: '@pagebob and @PageCat, and @pageeve who is not here, and me @pageann' })
    // Only members, never the sender, names in any case
    assert.deepEqual([...sent.json.mentions].sort((a, b) => a - b), [bob.id, cat.id].sort((a, b) => a - b))

    const listed = (await bob.get('/api/chat/channels')).json.channels.find((c) => c.id === group)
    assert.equal(listed.unreadCount, 1)
    assert.equal(listed.mentionCount, 1)
    await bob.post(`/api/chat/channels/${group}/read`, { lastReadId: sent.json.id })
    assert.equal((await bob.get('/api/chat/channels')).json.channels.find((c) => c.id === group).mentionCount, 0)

    // Nobody had a page open: a mail to cat, none to bob who didn't want them
    // Mails are written after the answer: waited for
    let mail
    for (const until = Date.now() + 3000; Date.now() < until; await new Promise((resolve) => setTimeout(resolve, 20))) {
      mail = lastMailTo(server.outbox, 'pagecat@example.com')
      if (mail.text.includes('提到了你'))
        break
    }
    assert.match(mail.text, /^pagecat Name，你好：/)
    assert.match(mail.text, /pageann Name 在「Garden」里提到了你/)
    assert.match(mail.text, /\/zh\/chat\?c=group:/)
    assert.deepEqual(lastMailTo(server.outbox, 'pagebob@example.com'), bobMails)

    // Someone with a page open gets none
    const page = await openPage(dan)
    const before = lastMailTo(server.outbox, 'pagedan@example.com')
    const toDan = await ann.post(`/api/chat/channels/${dm(ann, dan)}/messages`, { content: 'hi @pagedan' })
    assert.deepEqual(toDan.json.mentions, [dan.id])
    await page.waitFor((m) => m.type === 'chat_message' && m.id === toDan.json.id)
    await new Promise((resolve) => setTimeout(resolve, 300))
    assert.deepEqual(lastMailTo(server.outbox, 'pagedan@example.com'), before)
    page.close()
  })

  test('search finds messages only in chats the caller may read', async () => {
    await bob.post(`/api/chat/channels/${group}/messages`, { content: 'the secret garden key is under the pot' })
    await ann.post(`/api/chat/channels/${dm(ann, dan)}/messages`, { content: 'the secret of the pot, for dan' })
    await eve.post('/api/chat/channels/square/messages', { content: 'a public pot' })

    const forBob = await bob.get('/api/chat/search?q=pot')
    assert.equal(forBob.status, 200)
    assert.deepEqual(forBob.json.messages.map((m) => m.content), ['a public pot', 'the secret garden key is under the pot'])
    assert.equal(forBob.json.hasMore, false)

    const forEve = await eve.get('/api/chat/search?q=pot')
    assert.deepEqual(forEve.json.messages.map((m) => m.content), ['a public pot'])

    const forAnn = await ann.get(`/api/chat/search?q=SECRET&channelId=${encodeURIComponent(dm(ann, dan))}`)
    assert.deepEqual(forAnn.json.messages.map((m) => m.content), ['the secret of the pot, for dan'])

    // Wildcards are themselves; another's chat is refused
    assert.deepEqual((await ann.get('/api/chat/search?q=%25')).json.messages, [])
    assert.equal((await eve.get(`/api/chat/search?q=pot&channelId=${group}`)).status, 403)
    assert.equal((await eve.get('/api/chat/search?q=')).status, 400)
    // A file by its name
    assert.ok((await bob.get('/api/chat/search?q=dot.png')).json.messages.some((m) => m.file?.name === 'dot.png'))
  })

  test('pinned and muted chats: a muted one\'s unread leave the total, its mentions stay', async () => {
    const totalBefore = (await cat.get('/api/chat/unread')).json.count
    assert.equal((await cat.put(`/api/chat/channels/${group}/prefs`, { muted: true })).status, 200)
    assert.equal((await cat.put(`/api/chat/channels/${group}/prefs`, { pinned: true })).status, 200)
    const listed = (await cat.get('/api/chat/channels')).json.channels.find((c) => c.id === group)
    assert.equal(listed.pinned, true)
    assert.equal(listed.muted, true)

    const unread = listed.unreadCount
    await bob.post(`/api/chat/channels/${group}/messages`, { content: 'quiet one' })
    assert.equal((await cat.get('/api/chat/unread')).json.count, totalBefore - unread + listed.mentionCount)
    await bob.post(`/api/chat/channels/${group}/messages`, { content: 'loud one @pagecat' })
    assert.equal((await cat.get('/api/chat/unread')).json.count, totalBefore - unread + listed.mentionCount + 1)

    assert.equal((await cat.put(`/api/chat/channels/${group}/prefs`, { muted: false })).status, 200)
    const after = (await cat.get('/api/chat/channels')).json.channels.find((c) => c.id === group)
    assert.equal(after.pinned, true)
    assert.equal(after.muted, false)
    // Only chats one is in
    assert.equal((await eve.put(`/api/chat/channels/${group}/prefs`, { pinned: true })).status, 403)
  })

  test('the owner renames the group, adds friends, puts members out and hands it over', async () => {
    assert.equal((await bob.patch(`/api/chat/groups/${group.slice(6)}`, { name: 'Mine' })).json.error.code, 'not_owner')
    assert.equal((await ann.patch(`/api/chat/groups/${group.slice(6)}`, { name: 'Rose Garden' })).status, 200)
    assert.equal((await cat.get('/api/chat/channels')).json.channels.find((c) => c.id === group).name, 'Rose Garden')

    // Friends of the owner only
    assert.equal((await ann.post(`/api/chat/groups/${group.slice(6)}/members`, { memberIds: [eve.id] })).json.error.code, 'not_friends')
    const page = await openPage(dan)
    assert.equal((await ann.post(`/api/chat/groups/${group.slice(6)}/members`, { memberIds: [dan.id] })).status, 200)
    await page.waitFor((m) => m.type === 'chat_channels_changed')
    assert.ok((await dan.get('/api/chat/channels')).json.channels.some((c) => c.id === group))

    assert.equal((await bob.delete(`/api/chat/groups/${group.slice(6)}/members/${dan.id}`)).status, 403)
    assert.equal((await ann.delete(`/api/chat/groups/${group.slice(6)}/members/${dan.id}`)).status, 200)
    assert.ok(!(await dan.get('/api/chat/channels')).json.channels.some((c) => c.id === group))
    assert.equal((await dan.get(`/api/chat/channels/${group}/messages`)).status, 403)
    page.close()

    assert.equal((await ann.post(`/api/chat/groups/${group.slice(6)}/owner`, { userId: eve.id })).json.error.code, 'not_member')
    assert.equal((await ann.post(`/api/chat/groups/${group.slice(6)}/owner`, { userId: bob.id })).status, 200)
    const members = (await ann.get(`/api/chat/groups/${group.slice(6)}/members`)).json.members
    assert.equal(members.find((m) => m.id === bob.id).role, 'owner')
    assert.equal(members.find((m) => m.id === ann.id).role, 'member')
    assert.equal((await ann.get('/api/chat/channels')).json.channels.find((c) => c.id === group).ownerId, bob.id)
    // Ann may leave now without dissolving it
    assert.equal((await ann.post(`/api/chat/groups/${group.slice(6)}/leave`)).status, 200)
    assert.ok((await bob.get('/api/chat/channels')).json.channels.some((c) => c.id === group))
  })

  test('the files go from the disk with their group, and with an account purged', async () => {
    const owner = bob
    await sendFile(owner, group, png, 'last.png')
    const before = chatFiles().length
    assert.equal((await owner.post(`/api/chat/groups/${group.slice(6)}/leave`)).status, 200)
    assert.ok(chatFiles().length < before)

    const files = chatFiles().length
    await sendFile(dan, dm(ann, dan), png, 'dan.png')
    assert.equal(chatFiles().length, files + 1)
    const admin = new Client()
    assert.equal((await admin.post('/api/auth/login', { login: 'author', password: adminPassword, totp: totp(server.adminTotpSecret) })).status, 200)
    assert.equal((await admin.delete(`/api/admin/users/${dan.id}?mode=purge`)).status, 200)
    assert.equal(chatFiles().length, files)
  })

  test('the file route is no way around: a made-up id, a recalled or deleted message', async () => {
    assert.equal((await ann.binary('GET', '/api/chat/files/0')).status, 404)
    assert.equal((await ann.binary('GET', '/api/chat/files/abc')).status, 404)
    const text = await ann.post('/api/chat/channels/square/messages', { content: 'no file here' })
    assert.equal((await ann.binary('GET', `/api/chat/files/${text.json.id}`)).status, 404)
    assert.ok(baseUrl)
  })
})
