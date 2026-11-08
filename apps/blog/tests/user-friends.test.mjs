// User-to-user friendships and requests, against a real server (pnpm test:api).

import assert from 'node:assert/strict'
import { after, before, describe, test } from 'node:test'
import { Client, lastMailTo, startServer } from './server.mjs'

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

describe('user friendships', () => {
  let alice
  let bob
  let charlie
  let dave

  before(async () => {
    alice = await makeUser('alice')
    bob = await makeUser('bob')
    charlie = await makeUser('charlie')
    dave = await makeUser('dave')
  })

  test('cannot call when not logged in', async () => {
    const anon = new Client()
    assert.equal((await anon.get('/api/user/friends')).status, 401)
    assert.equal((await anon.get('/api/user/friends/requests')).status, 401)
    assert.equal((await anon.post('/api/user/friends/requests', { to: 'bob' })).status, 401)
  })

  test('friendship status before request', async () => {
    const statusSelf = await alice.get('/api/user/friends/status?username=alice')
    assert.equal(statusSelf.status, 200)
    assert.equal(statusSelf.json.status, 'self')

    const statusBob = await alice.get('/api/user/friends/status?username=bob')
    assert.equal(statusBob.status, 200)
    assert.equal(statusBob.json.status, 'none')
  })

  test('request validation: self, not found, empty', async () => {
    assert.equal((await alice.post('/api/user/friends/requests', { to: 'alice' })).status, 400)
    assert.equal((await alice.post('/api/user/friends/requests', { to: 'nonexistent_user_123' })).status, 404)
    assert.equal((await alice.post('/api/user/friends/requests', { to: '' })).status, 400)
  })

  test('send, check pending, and reject request', async () => {
    const send = await alice.post('/api/user/friends/requests', { to: 'bob', message: 'Hi Bob, be my friend!' })
    assert.equal(send.status, 200)
    assert.equal(send.json.ok, true)

    // Cannot send duplicate
    assert.equal((await alice.post('/api/user/friends/requests', { to: 'bob' })).status, 409)

    // Status check
    const aliceCheck = await alice.get('/api/user/friends/status?username=bob')
    assert.equal(aliceCheck.json.status, 'pending_sent')
    assert.ok(aliceCheck.json.requestId)

    const bobCheck = await bob.get('/api/user/friends/status?username=alice')
    assert.equal(bobCheck.json.status, 'pending_received')
    assert.equal(bobCheck.json.requestId, aliceCheck.json.requestId)

    // Requests lists
    const aliceReqs = await alice.get('/api/user/friends/requests')
    assert.equal(aliceReqs.json.incoming.length, 0)
    assert.equal(aliceReqs.json.outgoing.length, 1)
    assert.equal(aliceReqs.json.outgoing[0].user.username, 'bob')
    assert.equal(aliceReqs.json.outgoing[0].message, 'Hi Bob, be my friend!')

    const bobReqs = await bob.get('/api/user/friends/requests')
    assert.equal(bobReqs.json.incoming.length, 1)
    assert.equal(bobReqs.json.outgoing.length, 0)
    assert.equal(bobReqs.json.incoming[0].user.username, 'alice')
    assert.equal(bobReqs.json.incoming[0].message, 'Hi Bob, be my friend!')

    // Bob rejects
    const reject = await bob.delete(`/api/user/friends/requests/${bobReqs.json.incoming[0].id}`)
    assert.equal(reject.status, 200)

    assert.equal((await alice.get('/api/user/friends/status?username=bob')).json.status, 'none')
    assert.equal((await bob.get('/api/user/friends/status?username=alice')).json.status, 'none')
    assert.equal((await bob.get('/api/user/friends/requests')).json.incoming.length, 0)
  })

  test('send, accept, check friends, unfriend', async () => {
    // Alice sends again
    await alice.post('/api/user/friends/requests', { to: 'bob', message: 'Please Bob!' })
    const bobReqs = await bob.get('/api/user/friends/requests')
    const reqId = bobReqs.json.incoming[0].id

    // Bob accepts
    const accept = await bob.post(`/api/user/friends/requests/${reqId}/accept`)
    assert.equal(accept.status, 200)

    // Status check
    assert.equal((await alice.get('/api/user/friends/status?username=bob')).json.status, 'friends')
    assert.equal((await bob.get('/api/user/friends/status?username=alice')).json.status, 'friends')

    // Friends lists
    const aliceFriends = await alice.get('/api/user/friends')
    assert.equal(aliceFriends.json.friends.length, 1)
    assert.equal(aliceFriends.json.friends[0].username, 'bob')
    assert.ok(aliceFriends.json.friends[0].since)

    const bobFriends = await bob.get('/api/user/friends')
    assert.equal(bobFriends.json.friends.length, 1)
    assert.equal(bobFriends.json.friends[0].username, 'alice')
    assert.ok(bobFriends.json.friends[0].since)

    // Cannot send friend request to existing friend
    assert.equal((await alice.post('/api/user/friends/requests', { to: 'bob' })).status, 409)

    // Alice unfriends Bob
    const bobId = aliceFriends.json.friends[0].id
    const unfriend = await alice.delete(`/api/user/friends/${bobId}`)
    assert.equal(unfriend.status, 200)

    // Both are no longer friends
    assert.equal((await alice.get('/api/user/friends')).json.friends.length, 0)
    assert.equal((await bob.get('/api/user/friends')).json.friends.length, 0)
    assert.equal((await alice.get('/api/user/friends/status?username=bob')).json.status, 'none')
  })

  test('mutual request auto-accepts', async () => {
    // Charlie sends to Dave
    await charlie.post('/api/user/friends/requests', { to: 'dave', message: 'Hey Dave' })
    assert.equal((await charlie.get('/api/user/friends/status?username=dave')).json.status, 'pending_sent')

    // Dave sends to Charlie -> auto-accepts
    const mutual = await dave.post('/api/user/friends/requests', { to: 'charlie', message: 'Hey Charlie' })
    assert.equal(mutual.status, 200)

    // Both are friends now
    assert.equal((await charlie.get('/api/user/friends/status?username=dave')).json.status, 'friends')
    assert.equal((await dave.get('/api/user/friends/status?username=charlie')).json.status, 'friends')

    // Requests are cleared
    assert.equal((await charlie.get('/api/user/friends/requests')).json.outgoing.length, 0)
    assert.equal((await dave.get('/api/user/friends/requests')).json.incoming.length, 0)
  })

  test('sender can cancel their own pending request', async () => {
    // Alice sends to Charlie
    const send = await alice.post('/api/user/friends/requests', { to: 'charlie', message: 'Hello' })
    assert.equal(send.status, 200)

    const aliceReqs = await alice.get('/api/user/friends/requests')
    const reqId = aliceReqs.json.outgoing[0].id

    // Alice cancels
    const cancel = await alice.delete(`/api/user/friends/requests/${reqId}`)
    assert.equal(cancel.status, 200)

    assert.equal((await alice.get('/api/user/friends/requests')).json.outgoing.length, 0)
    assert.equal((await charlie.get('/api/user/friends/requests')).json.incoming.length, 0)
    assert.equal((await alice.get('/api/user/friends/status?username=charlie')).json.status, 'none')
  })

  test('send by user id', async () => {
    const bobMe = await bob.get('/api/auth/me')
    const bobId = bobMe.json.user.id

    const send = await charlie.post('/api/user/friends/requests', { to: String(bobId), message: 'ById' })
    assert.equal(send.status, 200)

    const bobReqs = await bob.get('/api/user/friends/requests')
    assert.equal(bobReqs.json.incoming[0].user.username, 'charlie')
    await bob.delete(`/api/user/friends/requests/${bobReqs.json.incoming[0].id}`)
  })
})

