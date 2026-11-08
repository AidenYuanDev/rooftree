// Real-time chat: square public channel, direct friend chats, FIFO trimming, unread tracking (pnpm test:api).

import assert from 'node:assert/strict'
import { after, before, describe, test } from 'node:test'
import { adminPassword, Client, lastMailTo, origin, startServer, totp, wsUrl } from './server.mjs'

let server

before(async () => {
  server = await startServer()
})

after(async () => {
  if (process.exitCode && server)
    console.log(server.log.join(''))
  await server?.stop()
})

async function makeUser(name) {
  const client = new Client()
  const email = `${name}@example.com`
  assert.equal((await client.post('/api/auth/register', { username: name, displayName: `${name} Name`, email, password: `password ${name}` })).status, 201)
  assert.equal((await client.post('/api/auth/verify-email', { token: lastMailTo(server.outbox, email).token })).status, 200)
  assert.equal((await client.post('/api/auth/login', { login: name, password: `password ${name}` })).status, 200)
  return client
}

// A page's WebSocket, as client (none: nobody logged in) watching targets; ready once the server
// counted it (its first presence message, the login looked up by then)
async function openPage(client, watch = []) {
  const headers = { Origin: origin }
  if (client?.cookies.size)
    headers.Cookie = [...client.cookies].map(([k, v]) => `${k}=${v}`).join('; ')
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
  socket.send(JSON.stringify({ type: 'page', path: '/zh', watch }))
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

const settle = () => new Promise((resolve) => setTimeout(resolve, 300))

describe('chat system', () => {
  let userA
  let userB
  let userC
  let userAId
  let userBId
  let userCId
  let admin

  before(async () => {
    userA = await makeUser('chata')
    userB = await makeUser('chatb')
    userC = await makeUser('chatc')
    admin = new Client()
    assert.equal((await admin.post('/api/auth/login', { login: 'author', password: adminPassword, totp: totp(server.adminTotpSecret) })).status, 200)

    const meA = await userA.get('/api/auth/me')
    userAId = meA.json.user.id
    const meB = await userB.get('/api/auth/me')
    userBId = meB.json.user.id
    const meC = await userC.get('/api/auth/me')
    userCId = meC.json.user.id
  })

  test('cannot call chat API when not logged in', async () => {
    const anon = new Client()
    assert.equal((await anon.get('/api/chat/channels')).status, 401)
    assert.equal((await anon.get('/api/chat/channels/square/messages')).status, 401)
    assert.equal((await anon.post('/api/chat/channels/square/messages', { content: 'hello' })).status, 401)
    assert.equal((await anon.get('/api/chat/unread')).status, 401)
  })

  test('square channel message send, list, and pagination', async () => {
    // Initially channels list has square
    const chs = await userA.get('/api/chat/channels')
    assert.equal(chs.status, 200)
    assert.ok(chs.json.channels.some((c) => c.id === 'square' && c.type === 'square'))

    // Send empty content fails
    assert.equal((await userA.post('/api/chat/channels/square/messages', { content: '   ' })).status, 400)

    // User A sends message
    const send1 = await userA.post('/api/chat/channels/square/messages', { content: 'Hello Square from A!' })
    assert.equal(send1.status, 200)
    assert.equal(send1.json.content, 'Hello Square from A!')
    assert.equal(send1.json.senderId, userAId)
    assert.ok(send1.json.id)

    // User B sends message
    const send2 = await userB.post('/api/chat/channels/square/messages', { content: 'Hello Square from B!' })
    assert.equal(send2.status, 200)

    // User C reads square messages
    const msgs = await userC.get('/api/chat/channels/square/messages')
    assert.equal(msgs.status, 200)
    assert.equal(msgs.json.messages.length, 2)
    assert.equal(msgs.json.messages[0].content, 'Hello Square from A!')
    assert.equal(msgs.json.messages[1].content, 'Hello Square from B!')

    // Pagination before send2.id
    const page = await userC.get(`/api/chat/channels/square/messages?before=${send2.json.id}&limit=10`)
    assert.equal(page.status, 200)
    assert.equal(page.json.messages.length, 1)
    assert.equal(page.json.messages[0].id, send1.json.id)
  })

  test('direct chat requires mutual friendship', async () => {
    const minId = Math.min(userAId, userCId)
    const maxId = Math.max(userAId, userCId)
    const dmId = `dm:${minId}:${maxId}`

    // A and C are not friends yet: cannot read or send
    assert.equal((await userA.get(`/api/chat/channels/${dmId}/messages`)).status, 403)
    assert.equal((await userA.post(`/api/chat/channels/${dmId}/messages`, { content: 'Secret' })).status, 403)

    // A requests C, C accepts
    await userA.post('/api/user/friends/requests', { to: 'chatc' })
    const cReqs = await userC.get('/api/user/friends/requests')
    await userC.post(`/api/user/friends/requests/${cReqs.json.incoming[0].id}/accept`)

    // Now they are friends: channels list includes dmId
    const aChannels = await userA.get('/api/chat/channels')
    const dmChan = aChannels.json.channels.find((c) => c.id === dmId)
    assert.ok(dmChan)
    assert.equal(dmChan.type, 'direct')
    assert.equal(dmChan.friend.username, 'chatc')

    // A sends DM to C
    const dmMsg = await userA.post(`/api/chat/channels/${dmId}/messages`, { content: 'Private message from A to C' })
    assert.equal(dmMsg.status, 200)

    // C reads DM
    const cMsgs = await userC.get(`/api/chat/channels/${dmId}/messages`)
    assert.equal(cMsgs.status, 200)
    assert.equal(cMsgs.json.messages.length, 1)
    assert.equal(cMsgs.json.messages[0].content, 'Private message from A to C')

    // C has unread count
    const cChannels = await userC.get('/api/chat/channels')
    const cDmChan = cChannels.json.channels.find((c) => c.id === dmId)
    assert.equal(cDmChan.unreadCount, 1)

    // C marks read
    const mark = await userC.post(`/api/chat/channels/${dmId}/read`, { lastReadId: dmMsg.json.id })
    assert.equal(mark.status, 200)

    // Unread count is now 0
    const cChannelsAfter = await userC.get('/api/chat/channels')
    const cDmChanAfter = cChannelsAfter.json.channels.find((c) => c.id === dmId)
    assert.equal(cDmChanAfter.unreadCount, 0)
  })

  test('unread total count', async () => {
    const unread = await userB.get('/api/chat/unread')
    assert.equal(unread.status, 200)
    assert.equal(typeof unread.json.count, 'number')
  })

  test('a read cursor past the newest message stops there', async () => {
    // A made-up id (the dock once sent one for a message still being sent) must not hide the next ones
    assert.equal((await userB.post('/api/chat/channels/square/read', { channelId: 'square', lastReadId: 1_761_900_000_000 })).status, 200)
    const before = (await userB.get('/api/chat/unread')).json.count
    assert.equal((await userA.post('/api/chat/channels/square/messages', { content: 'after the bogus cursor' })).status, 200)
    assert.equal((await userB.get('/api/chat/unread')).json.count, before + 1)
    const last = (await userB.get('/api/chat/channels/square/messages')).json.messages.at(-1)
    assert.equal((await userB.post('/api/chat/channels/square/read', { channelId: 'square', lastReadId: last.id })).status, 200)
    assert.equal((await userB.get('/api/chat/unread')).json.count, before)
  })

  test('a message reaches its readers only, whatever others watch', async () => {
    const dmId = `dm:${Math.min(userAId, userCId)}:${Math.max(userAId, userCId)}`
    const nobody = await openPage(null, ['chat:square', `chat:${dmId}`])
    const outsider = await openPage(userB, [`chat:${dmId}`])
    const friend = await openPage(userC)

    await userA.post('/api/chat/channels/square/messages', { content: 'Realtime Square Test' })
    const msg = await friend.waitFor((m) => m.type === 'chat_message' && m.content === 'Realtime Square Test')
    assert.equal(msg.channelId, 'square')
    assert.equal(msg.senderId, userAId)
    await outsider.waitFor((m) => m.type === 'chat_message' && m.content === 'Realtime Square Test')

    await userA.post(`/api/chat/channels/${dmId}/messages`, { content: 'Only for C' })
    await friend.waitFor((m) => m.type === 'chat_message' && m.content === 'Only for C')
    await settle()
    // Watching tells that something changed, never what
    assert.ok(!outsider.received.some((m) => m.type === 'chat_message' && m.channelId === dmId))
    assert.ok(!nobody.received.some((m) => m.type === 'chat_message'))
    for (const page of [nobody, outsider, friend])
      page.close()
  })

  test('a direct chat has one id, either way round', async () => {
    const lower = Math.min(userAId, userCId)
    const higher = Math.max(userAId, userCId)
    const sent = await userA.post(`/api/chat/channels/dm:${higher}:${lower}/messages`, { content: 'the other way round' })
    assert.equal(sent.status, 200)
    assert.equal(sent.json.channelId, `dm:${lower}:${higher}`)
    const listed = (await userC.get(`/api/chat/channels/dm:${lower}:${higher}/messages`)).json.messages
    assert.equal(listed.at(-1).content, 'the other way round')
  })

  test('message and group name checked in characters', async () => {
    assert.equal((await userA.post('/api/chat/channels/square/messages', { content: '字'.repeat(1000) })).status, 200)
    const tooLong = await userA.post('/api/chat/channels/square/messages', { content: '字'.repeat(1001) })
    assert.deepEqual([tooLong.status, tooLong.json.error.fields], [400, { content: 'too_long' }])
    const control = await userA.post('/api/chat/channels/square/messages', { content: 'a\u0007b' })
    assert.deepEqual(control.json.error.fields, { content: 'invalid' })
    const name = await userA.post('/api/chat/groups', { name: '群'.repeat(51), memberIds: [] })
    assert.deepEqual(name.json.error.fields, { name: 'too_long' })
    const ok = await userA.post('/api/chat/groups', { name: '群'.repeat(50), memberIds: [] })
    assert.equal(ok.json.name, '群'.repeat(50))
    assert.equal((await userA.post(`/api/chat/groups/${ok.json.id.slice(6)}/leave`)).status, 200)
  })

  test('custom group chat create, chat, members, and leave', async () => {
    // A creates a group with friend C
    const create = await userA.post('/api/chat/groups', { name: 'Book Club', memberIds: [userCId] })
    assert.equal(create.status, 200)
    assert.equal(create.json.name, 'Book Club')
    assert.equal(create.json.type, 'group')
    assert.equal(create.json.memberCount, 2)
    const groupId = Number(create.json.id.replace('group:', ''))
    const chanId = create.json.id

    // Non-member B cannot read or post
    assert.equal((await userB.get(`/api/chat/channels/${chanId}/messages`)).status, 403)
    assert.equal((await userB.post(`/api/chat/channels/${chanId}/messages`, { content: 'Intruder' })).status, 403)

    // A posts in group
    const msg1 = await userA.post(`/api/chat/channels/${chanId}/messages`, { content: 'Welcome to Book Club!' })
    assert.equal(msg1.status, 200)

    // C sees group in channels and reads message
    const cChannels = await userC.get('/api/chat/channels')
    const cGrp = cChannels.json.channels.find((ch) => ch.id === chanId)
    assert.ok(cGrp)
    assert.equal(cGrp.name, 'Book Club')
    assert.equal(cGrp.memberCount, 2)

    const cMsgs = await userC.get(`/api/chat/channels/${chanId}/messages`)
    assert.equal(cMsgs.status, 200)
    assert.equal(cMsgs.json.messages.length, 1)
    assert.equal(cMsgs.json.messages[0].content, 'Welcome to Book Club!')

    // Members list
    const members = await userA.get(`/api/chat/groups/${groupId}/members`)
    assert.equal(members.status, 200)
    assert.equal(members.json.members.length, 2)
    assert.ok(members.json.members.some((m) => m.role === 'owner' && m.id === userAId))
    assert.ok(members.json.members.some((m) => m.role === 'member' && m.id === userCId))

    // C leaves group
    const leave = await userC.post(`/api/chat/groups/${groupId}/leave`)
    assert.equal(leave.status, 200)

    // C no longer sees group in channels
    const cChannelsAfter = await userC.get('/api/chat/channels')
    assert.ok(!cChannelsAfter.json.channels.some((ch) => ch.id === chanId))
  })
  test('a group its owner leaves goes, its messages with it; its members are told', async () => {
    const group = (await userA.post('/api/chat/groups', { name: 'Short lived', memberIds: [userCId] })).json
    const groupId = Number(group.id.slice(6))
    await userA.post(`/api/chat/channels/${group.id}/messages`, { content: 'soon gone' })
    const member = await openPage(userC)
    assert.equal((await userA.post(`/api/chat/groups/${groupId}/leave`)).status, 200)
    await member.waitFor((m) => m.type === 'chat_channels_changed')
    await member.waitFor((m) => m.type === 'chat_removed' && m.channelId === group.id)
    member.close()
    assert.ok(!(await userC.get('/api/chat/channels')).json.channels.some((ch) => ch.id === group.id))
    assert.equal((await userC.get(`/api/chat/channels/${group.id}/messages`)).status, 403)
    assert.ok(!(await admin.get('/api/admin/chat/groups')).json.groups.some((g) => g.id === groupId))
  })

  test('chat taken from an account, on its own', async () => {
    assert.equal((await admin.patch(`/api/admin/users/${userBId}`, { denied: ['chat'] })).status, 200)
    assert.equal((await userB.post('/api/chat/channels/square/messages', { content: 'muted' })).status, 403)
    assert.equal((await userB.post('/api/chat/groups', { name: 'muted', memberIds: [] })).status, 403)
    assert.equal((await userB.get('/api/chat/channels/square/messages')).status, 200)
    assert.equal((await admin.patch(`/api/admin/users/${userBId}`, { denied: ['comment'] })).status, 200)
    assert.equal((await userB.post('/api/chat/channels/square/messages', { content: 'may chat, not comment' })).status, 200)
    assert.equal((await admin.patch(`/api/admin/users/${userBId}`, { denied: [] })).status, 200)
  })

  test('moderators remove square and group messages, never see direct chats', async () => {
    assert.equal((await userA.get('/api/admin/chat/messages?channel=square')).status, 403)
    assert.equal((await userA.delete('/api/admin/chat/channels/square/messages')).status, 403)

    const spam = (await userB.post('/api/chat/channels/square/messages', { content: 'spam spam' })).json
    const page = await openPage(userC)
    const listed = (await admin.get('/api/admin/chat/messages?channel=square')).json
    assert.equal(listed.messages[0].id, spam.id)
    assert.equal(listed.messages[0].senderUsername, 'chatb')
    assert.deepEqual((await admin.delete(`/api/admin/chat/messages/${spam.id}`)).json, { removed: 1 })
    await page.waitFor((m) => m.type === 'chat_removed' && m.id === spam.id)
    page.close()
    assert.ok(!(await userC.get('/api/chat/channels/square/messages')).json.messages.some((m) => m.id === spam.id))

    const dmId = `dm:${Math.min(userAId, userCId)}:${Math.max(userAId, userCId)}`
    const secret = (await userA.post(`/api/chat/channels/${dmId}/messages`, { content: 'between us' })).json
    assert.deepEqual((await admin.get(`/api/admin/chat/messages?channel=${dmId}`)).json.error.fields, { channel: 'invalid' })
    assert.equal((await admin.delete(`/api/admin/chat/messages/${secret.id}`)).status, 404)
    assert.equal((await admin.delete(`/api/admin/chat/channels/${dmId}/messages`)).status, 404)

    // A moderator may too
    assert.equal((await admin.patch(`/api/admin/users/${userCId}`, { role: 'moderator' })).status, 200)
    const cleared = await userC.delete('/api/admin/chat/channels/square/messages')
    assert.equal(cleared.status, 200)
    assert.ok(cleared.json.removed > 0)
    assert.deepEqual((await userA.get('/api/chat/channels/square/messages')).json.messages, [])
    assert.equal((await admin.patch(`/api/admin/users/${userCId}`, { role: 'user' })).status, 200)
  })
})
