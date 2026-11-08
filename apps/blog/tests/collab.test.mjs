// A post's editors writing together over the WebSocket (Collab.h) against a real server (pnpm test:api):
// the order of their changes, what a page that was away gets, the working copy and publishing it.

import assert from 'node:assert/strict'
import { after, before, describe, test } from 'node:test'
import { adminPassword, Client, lastMailTo, origin, startServer, totp, wsUrl } from './server.mjs'

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

async function user(name) {
  const client = new Client()
  const email = `${name}@example.com`
  assert.equal((await client.post('/api/auth/register', { username: name, displayName: `${name} 名`, email, password: `password ${name}` })).status, 201)
  assert.equal((await client.post('/api/auth/verify-email', { token: lastMailTo(server.outbox, email).token })).status, 200)
  assert.equal((await client.post('/api/auth/login', { login: name, password: `password ${name}` })).status, 200)
  client.id = (await client.get('/api/auth/me')).json.user.id
  return client
}

// A page's connection, logged in as client; take(predicate) waits for the next "doc" message that
// matches and takes it, so one message is matched once
async function page(client, id) {
  const headers = { Origin: origin }
  if (client) headers.Cookie = [...client.cookies].map(([k, v]) => `${k}=${v}`).join('; ')
  const socket = new WebSocket(wsUrl, { headers })
  const received = []
  const waiting = []
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data)
    if (message.type !== 'doc') return
    const waiter = waiting.find((item) => item.predicate(message))
    if (waiter) {
      waiting.splice(waiting.indexOf(waiter), 1)
      waiter.resolve(message)
    } else received.push(message)
  })
  await new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve)
    socket.addEventListener('error', reject)
  })
  return {
    id,
    socket,
    send: (message) => socket.send(JSON.stringify({ type: 'doc', ...message })),
    take: (predicate, timeout = 4000) => {
      const index = received.findIndex(predicate)
      if (index !== -1) return Promise.resolve(received.splice(index, 1)[0])
      return new Promise((resolve, reject) => {
        waiting.push({ predicate, resolve })
        setTimeout(() => reject(new Error(`no such message, got ${JSON.stringify(received)}`)), timeout)
      })
    },
    // Nothing more arrives that matches within a while
    none: async (predicate, wait = 300) => {
      await new Promise((resolve) => setTimeout(resolve, wait))
      assert.equal(received.find(predicate), undefined)
    },
  }
}

const op = (name, doc) => (message) => message.op === name && (!doc || message.doc === doc)

describe('editors writing together', () => {
  let writer
  let editor
  let stranger
  let postId
  let body

  before(async () => {
    writer = await user('writer')
    editor = await user('editor')
    stranger = await user('stranger')
    assert.equal((await admin.put(`/api/admin/users/${writer.id}/write`, { enabled: true })).status, 200)
    const post = await writer.post('/api/admin/posts', {
      slug: 'together',
      status: 'published',
      originalLang: 'en',
      translations: { en: { title: 'Together', body: 'Hello world' } },
    })
    assert.equal(post.status, 201)
    postId = post.json.id
    body = `post:${postId}:en:body`
    assert.equal((await writer.put(`/api/admin/posts/${postId}/editors`, { username: 'editor', rights: [] })).status, 200)
  })

  test('only its editors open its documents', async () => {
    const anonymous = await page(null)
    anonymous.send({ op: 'open', doc: body, client: 'a' })
    assert.equal((await anonymous.take(op('error'))).code, 'login')
    anonymous.socket.close()

    const outsider = await page(stranger)
    outsider.send({ op: 'open', doc: body, client: 's' })
    assert.equal((await outsider.take(op('error'))).code, 'not_found')
    // Pushing to what it never opened
    outsider.send({ op: 'push', doc: body, version: 0, updates: [{ changes: [11], client: 's' }] })
    assert.equal((await outsider.take(op('error'))).code, 'not_open')
    outsider.send({ op: 'open', doc: `post:${postId}:fr:body`, client: 's' })
    assert.equal((await outsider.take(op('error'))).code, 'invalid')
    outsider.socket.close()
  })

  test('changes in one order, a change on an older version refused, cursors shared', async () => {
    const one = await page(writer, 'w1')
    one.send({ op: 'open', doc: body, client: 'w1' })
    const state = await one.take(op('state', body))
    assert.deepEqual([state.version, state.text, state.peers], [0, 'Hello world', []])

    const two = await page(editor, 'e1')
    two.send({ op: 'open', doc: body, client: 'e1' })
    const joined = await two.take(op('state', body))
    assert.deepEqual(joined.peers.map((peer) => [peer.client, peer.user.username]), [['w1', 'writer']])
    assert.equal((await one.take(op('peer', body))).user.displayName, 'editor 名')

    // "Hello world" -> "Hello, world": both get it, the sender learns it was taken
    one.send({ op: 'push', doc: body, version: 0, updates: [{ changes: [5, [0, ','], 6], client: 'w1' }] })
    for (const p of [one, two]) {
      const updates = await p.take(op('updates', body))
      assert.deepEqual([updates.version, updates.updates], [0, [{ changes: [5, [0, ','], 6], client: 'w1' }]])
    }

    // Made on version 0, which isn't the document's any more
    two.send({ op: 'push', doc: body, version: 0, updates: [{ changes: [[5, 'Bye'], 6], client: 'e1' }] })
    await two.take(op('rejected', body))
    // On top of version 1, with a new line: "Hello, world" -> "Bye, world\n!"
    two.send({ op: 'push', doc: body, version: 1, updates: [{ changes: [[5, 'Bye'], 7, [0, '', '!']], client: 'e1' }] })
    assert.equal((await one.take(op('updates', body))).version, 1)
    await two.take(op('updates', body))

    // Changes that don't cover the text, or aren't changes
    one.send({ op: 'push', doc: body, version: 2, updates: [{ changes: [3], client: 'w1' }] })
    assert.equal((await one.take(op('error', body))).code, 'invalid')
    one.send({ op: 'push', doc: body, version: 2, updates: [{ changes: [[99]], client: 'w1' }] })
    assert.equal((await one.take(op('error', body))).code, 'invalid')

    two.send({ op: 'cursor', doc: body, selection: { anchor: 3, head: 5 } })
    const cursor = await one.take(op('peer', body))
    assert.deepEqual([cursor.client, cursor.selection], ['e1', { anchor: 3, head: 5 }])

    // A third page: the text as it is now; one that was away: what it missed
    const three = await page(writer, 'w2')
    three.send({ op: 'open', doc: body, client: 'w2' })
    const now = await three.take(op('state', body))
    assert.deepEqual([now.version, now.text], [2, 'Bye, world\n!'])
    three.send({ op: 'close', doc: body })
    three.send({ op: 'open', doc: body, client: 'w2', since: 1 })
    const missed = await three.take(op('updates', body))
    assert.deepEqual([missed.version, missed.updates.map((update) => update.client)], [1, ['e1']])
    three.socket.close()
    assert.equal((await one.take(op('left', body))).client, 'w2')

    one.socket.close()
    two.socket.close()
  })

  test('what the editors write is a working copy until someone publishes it', async () => {
    // Readers still have the text as published
    assert.equal((await writer.get(`/api/admin/posts/${postId}`)).json.translations.en.body, 'Hello world')
    // Written to the database within a second
    await new Promise((resolve) => setTimeout(resolve, 1500))

    assert.equal((await editor.post(`/api/admin/posts/${postId}/publish`)).status, 403)
    const published = await writer.post(`/api/admin/posts/${postId}/publish`)
    assert.equal(published.status, 200)
    assert.equal(published.json.translations.en.body, 'Bye, world\n!')
    assert.equal(published.json.translations.en.title, 'Together')
    assert.equal((await stranger.post(`/api/admin/posts/${postId}/publish`)).status, 404)
  })

  test('publishing takes the newest text, written or not', async () => {
    const one = await page(writer, 'w3')
    const title = `post:${postId}:en:title`
    one.send({ op: 'open', doc: title, client: 'w3' })
    assert.equal((await one.take(op('state', title))).text, 'Together')
    one.send({ op: 'push', doc: title, version: 0, updates: [{ changes: [8, [0, ' again']], client: 'w3' }] })
    await one.take(op('updates', title))
    // Right away, before the second it is written in
    const published = await writer.post(`/api/admin/posts/${postId}/publish`)
    assert.equal(published.json.translations.en.title, 'Together again')

    // Too long for a title
    one.send({ op: 'push', doc: title, version: 1, updates: [{ changes: [14, [0, 'x'.repeat(200)]], client: 'w3' }] })
    assert.equal((await one.take(op('error', title))).code, 'too_long')
    one.socket.close()
  })

  test('a language added in the working copy; a draft published with its working copy', async () => {
    const one = await page(writer, 'w4')
    const zhTitle = `post:${postId}:zh:title`
    const zhBody = `post:${postId}:zh:body`
    for (const doc of [zhTitle, zhBody]) {
      one.send({ op: 'open', doc, client: 'w4' })
      assert.deepEqual(await one.take(op('state', doc)).then((state) => [state.version, state.text]), [0, ''])
    }
    one.send({ op: 'push', doc: zhTitle, version: 0, updates: [{ changes: [[0, '一起']], client: 'w4' }] })
    await one.take(op('updates', zhTitle))
    // Its title only: no body to publish yet
    const missing = await writer.post(`/api/admin/posts/${postId}/publish`)
    assert.equal(missing.json.error.fields['translations.zh.body'], 'required')
    one.send({ op: 'push', doc: zhBody, version: 0, updates: [{ changes: [[0, '你好']], client: 'w4' }] })
    await one.take(op('updates', zhBody))
    const published = await writer.post(`/api/admin/posts/${postId}/publish`)
    assert.deepEqual(published.json.translations.zh, { title: '一起', summary: '', body: '你好' })
    one.socket.close()

    // A draft: published (by its status too) with what its editors wrote
    const draft = (await writer.post('/api/admin/posts', { slug: 'a-draft', status: 'draft', originalLang: 'en', translations: { en: { title: 'Draft', body: 'First' } } })).json
    const two = await page(writer, 'w5')
    const draftBody = `post:${draft.id}:en:body`
    two.send({ op: 'open', doc: draftBody, client: 'w5' })
    await two.take(op('state', draftBody))
    two.send({ op: 'push', doc: draftBody, version: 0, updates: [{ changes: [5, [0, ' and second']], client: 'w5' }] })
    await two.take(op('updates', draftBody))
    // Its list shows the working copy's title, its page the published text until then
    const status = await writer.patch(`/api/admin/posts/${draft.id}`, { status: 'published' })
    assert.equal(status.status, 200)
    assert.equal(status.json.translations.en.body, 'First and second')
    two.socket.close()
  })

  test('an editor removed loses its documents; text sent from elsewhere reloads them', async () => {
    const mine = await page(writer, 'w6')
    const theirs = await page(editor, 'e6')
    for (const p of [mine, theirs]) {
      p.send({ op: 'open', doc: body, client: p.id })
      await p.take(op('state', body))
    }
    await mine.take(op('peer', body))

    assert.equal((await writer.delete(`/api/admin/posts/${postId}/editors/${editor.id}`)).status, 200)
    assert.equal((await theirs.take(op('closed', body))).reason, 'removed')
    assert.equal((await mine.take(op('left', body))).client, 'e6')
    theirs.send({ op: 'push', doc: body, version: 0, updates: [{ changes: [1], client: 'e6' }] })
    assert.equal((await theirs.take(op('error', body))).code, 'not_open')

    // The text sent with an update replaces the working copy
    assert.equal((await writer.patch(`/api/admin/posts/${postId}`, { translations: { en: { title: 'Replaced', body: 'New' } } })).status, 200)
    assert.equal((await mine.take(op('closed', body))).reason, 'replaced')
    mine.send({ op: 'open', doc: body, client: 'w6' })
    assert.equal((await mine.take(op('state', body))).text, 'New')

    mine.socket.close()
    theirs.socket.close()
  })

  test('versions: each publication and each one saved, kept; one restored to the working copy', async () => {
    const all = async () => (await writer.get(`/api/admin/posts/${postId}/revisions`)).json
    const before = (await all()).revisions
    assert.ok(before.every((revision) => revision.kind === 'published' && revision.author.username === 'writer'))
    assert.equal(before[0].body, undefined)

    // Nothing changed since the last version: none saved
    const unchanged = await writer.post(`/api/admin/posts/${postId}/revisions`, { note: 'nothing' })
    assert.deepEqual([unchanged.status, unchanged.json.revisions], [200, []])

    // A change in the working copy, saved as a version with a note, by whoever edits it
    const one = await page(writer, 'w7')
    one.send({ op: 'open', doc: body, client: 'w7' })
    const state = await one.take(op('state', body))
    one.send({ op: 'push', doc: body, version: state.version, updates: [{ changes: [state.text.length, [0, ' draft']], client: 'w7' }] })
    await one.take(op('updates', body))
    const saved = await writer.post(`/api/admin/posts/${postId}/revisions`, { note: 'before the big change' })
    assert.equal(saved.status, 201)
    assert.deepEqual(saved.json.revisions.map((r) => [r.lang, r.kind, r.note, r.author.username]), [['en', 'draft', 'before the big change', 'writer']])
    const draftId = saved.json.revisions[0].id
    assert.equal((await stranger.post(`/api/admin/posts/${postId}/revisions`, {})).status, 404)

    // In full, with what it changed
    const full = (await writer.get(`/api/admin/posts/${postId}/revisions/${draftId}`)).json
    assert.equal(full.body, `${state.text} draft`)
    assert.equal(full.previous.body, state.text)
    assert.equal((await writer.get(`/api/admin/posts/${postId}/revisions/999999`)).status, 404)

    // An older version back: the pages open the working copy again, readers still have theirs
    const oldest = before.at(-1)
    const restored = await writer.post(`/api/admin/posts/${postId}/revisions/${oldest.id}/restore`)
    assert.equal(restored.status, 200)
    assert.equal((await one.take(op('closed', body))).reason, 'replaced')
    one.send({ op: 'open', doc: body, client: 'w7' })
    const back = await one.take(op('state', body))
    assert.equal(back.text, (await writer.get(`/api/admin/posts/${postId}/revisions/${oldest.id}`)).json.body)
    assert.ok(back.version > state.version)
    // Readers keep what was published until someone publishes it
    assert.equal(restored.json.translations.en.body, 'New')
    one.socket.close()

    // Published again: a new version, kept with the others
    await writer.post(`/api/admin/posts/${postId}/publish`)
    const after = (await all()).revisions
    assert.equal(after[0].kind, 'published')
    assert.equal(after.length, before.length + 2)
  })

  test('the console publishes the working copy', async () => {
    const one = await page(admin, 'a1')
    one.send({ op: 'open', doc: body, client: 'a1' })
    const state = await one.take(op('state', body))
    one.send({ op: 'push', doc: body, version: state.version, updates: [{ changes: [state.text.length, [0, ' from the console']], client: 'a1' }] })
    await one.take(op('updates', body))
    assert.match(server.console('post publish together'), /Published post together/)
    assert.equal((await admin.get(`/api/admin/posts/${postId}`)).json.translations.en.body, `${state.text} from the console`)
    assert.match(server.console('post save-version together nothing new'), /Nothing changed since its last version/)
    const versions = server.console('post revisions together')
    assert.match(versions, /^ +\d+ +en +published .* author +Together/)
    assert.match(versions, /en +draft .* writer .*: before the big change/)
    const oldest = versions.trim().split('\n').at(-1).trim().split(/ +/)[0]
    assert.match(server.console(`post restore together ${oldest}`), new RegExp(`Version ${oldest} is back in the working copy of together`))
    one.socket.close()
  })
})
