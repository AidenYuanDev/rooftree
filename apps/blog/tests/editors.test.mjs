// Who writes and edits posts against a real server (pnpm test:api): the admin gives an account
// writing, the owner of a post invites editors, an editor's rights decide what more it may change.

import assert from 'node:assert/strict'
import { after, before, describe, test } from 'node:test'
import { adminPassword, baseUrl, Client, lastMailTo, startServer, totp } from './server.mjs'

let server
let admin

before(async () => {
  server = await startServer()
  admin = new Client()
  const login = await admin.post('/api/auth/login', { login: 'author', password: adminPassword, totp: totp(server.adminTotpSecret) })
  assert.equal(login.status, 200)
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

const draft = (slug, title = 'A draft') => ({ slug, status: 'draft', originalLang: 'en', translations: { en: { title, body: 'Its text.' } } })

describe('writing and editing posts', () => {
  let writer
  let editor
  let stranger
  let postId

  before(async () => {
    writer = await user('writer')
    editor = await user('editor')
    stranger = await user('stranger')
  })

  test('the admin gives an account writing; it owns what it writes', async () => {
    assert.equal((await writer.get('/api/auth/me')).json.user.write, false)
    assert.equal((await writer.post('/api/admin/posts', draft('not-yet'))).status, 403)

    const given = await admin.put(`/api/admin/users/${writer.id}/write`, { enabled: true })
    assert.equal(given.status, 200)
    assert.equal(given.json.write, true)
    assert.equal((await writer.get('/api/auth/me')).json.user.write, true)

    const created = await writer.post('/api/admin/posts', draft('by-writer', 'By the writer'))
    assert.equal(created.status, 201)
    postId = created.json.id
    assert.equal(created.json.owner.username, 'writer')
    assert.deepEqual(created.json.rights, ['publish', 'settings', 'editors', 'delete'])

    // Its list has its own posts only; the admin's has every one
    const mine = (await writer.get('/api/admin/posts')).json.posts
    assert.deepEqual(mine.map((post) => [post.slug, post.mine]), [['by-writer', true]])
    assert.equal((await admin.post('/api/admin/posts', draft('by-admin'))).status, 201)
    assert.deepEqual((await admin.get('/api/admin/posts')).json.posts.map((post) => [post.slug, post.mine]), [['by-admin', true], ['by-writer', false]])

    // Someone else's post is no post to it, nor to anyone who doesn't edit it
    const adminPost = (await admin.get('/api/admin/posts')).json.posts[0].id
    assert.equal((await writer.get(`/api/admin/posts/${adminPost}`)).status, 404)
    assert.equal((await stranger.get(`/api/admin/posts/${postId}`)).status, 404)
    assert.equal((await stranger.patch(`/api/admin/posts/${postId}`, { translations: { en: { title: 'Mine', body: 'x' } } })).status, 404)
    assert.deepEqual((await stranger.get('/api/admin/posts')).json.posts, [])
    // Not the admin's pages: only posts
    assert.equal((await writer.get('/api/admin/users')).status, 403)
  })

  test('an invited editor edits the text, and more only with rights', async () => {
    const invited = await writer.put(`/api/admin/posts/${postId}/editors`, { username: 'editor', rights: [] })
    assert.equal(invited.status, 200)
    assert.equal(invited.json.owner.username, 'writer')
    assert.deepEqual(invited.json.editors.map((item) => [item.user.username, item.rights]), [['editor', []]])
    assert.equal((await writer.put(`/api/admin/posts/${postId}/editors`, { username: 'nobody' })).json.error.fields.username, 'not_found')
    assert.equal((await writer.put(`/api/admin/posts/${postId}/editors`, { username: 'writer' })).json.error.fields.username, 'owner')
    assert.equal((await writer.put(`/api/admin/posts/${postId}/editors`, { username: 'editor', rights: ['delete'] })).json.error.fields.rights, 'invalid')

    // It sees the post, in its list too, without writing of its own
    const post = await editor.get(`/api/admin/posts/${postId}`)
    assert.equal(post.status, 200)
    assert.deepEqual(post.json.rights, [])
    assert.deepEqual((await editor.get('/api/admin/posts')).json.posts.map((item) => [item.slug, item.mine]), [['by-writer', false]])

    // The text is its to change; the whole post sent back as it was is too
    const edited = await editor.patch(`/api/admin/posts/${postId}`, {
      slug: 'by-writer',
      status: 'draft',
      originalLang: 'en',
      cover: '',
      tags: [],
      translations: { en: { title: 'Edited', body: 'New text.' }, zh: { title: '译文', body: '新的文字。' } },
    })
    assert.equal(edited.status, 200)
    assert.equal(edited.json.translations.en.title, 'Edited')
    assert.equal(edited.json.translations.zh.title, '译文')

    // Publishing, settings, a language taken away, editors, deleting: not without rights
    const refused = await editor.patch(`/api/admin/posts/${postId}`, { status: 'published', slug: 'moved', translations: { zh: {} } })
    assert.equal(refused.status, 403)
    assert.deepEqual(refused.json.error.fields, { status: 'not_allowed', slug: 'not_allowed', 'translations.zh': 'not_allowed' })
    assert.equal((await editor.put(`/api/admin/posts/${postId}/editors`, { username: 'stranger' })).status, 403)
    assert.equal((await editor.delete(`/api/admin/posts/${postId}`)).status, 403)
    assert.equal((await editor.patch(`/api/admin/posts/${postId}`, { owner: 'editor' })).json.error.fields.owner, 'not_allowed')
    assert.equal((await writer.patch(`/api/admin/posts/${postId}`, { owner: 'editor' })).json.error.fields.owner, 'not_allowed')

    // Given publish: it publishes, still no settings
    await writer.put(`/api/admin/posts/${postId}/editors`, { username: 'editor', rights: ['publish'] })
    assert.deepEqual((await editor.get(`/api/admin/posts/${postId}`)).json.rights, ['publish'])
    assert.equal((await editor.patch(`/api/admin/posts/${postId}`, { status: 'published' })).json.status, 'published')
    assert.equal((await editor.patch(`/api/admin/posts/${postId}`, { cover: 'https://example.com/a.png' })).status, 403)
    assert.equal((await editor.get(`/api/admin/posts/${postId}/revisions`)).status, 200)
    assert.equal((await stranger.get(`/api/admin/posts/${postId}/revisions`)).status, 404)
  })

  test('the editors right invites others with no more rights than its own', async () => {
    await writer.put(`/api/admin/posts/${postId}/editors`, { username: 'editor', rights: ['publish', 'editors'] })
    assert.equal((await editor.put(`/api/admin/posts/${postId}/editors`, { username: 'stranger', rights: ['settings'] })).status, 403)
    const added = await editor.put(`/api/admin/posts/${postId}/editors`, { username: 'stranger', rights: ['publish'] })
    assert.equal(added.status, 200)
    assert.deepEqual(added.json.editors.map((item) => item.user.username), ['editor', 'stranger'])
    assert.equal((await stranger.get(`/api/admin/posts/${postId}`)).status, 200)

    // Who has more than it isn't its to change
    await writer.put(`/api/admin/posts/${postId}/editors`, { username: 'stranger', rights: ['settings'] })
    assert.equal((await editor.delete(`/api/admin/posts/${postId}/editors/${stranger.id}`)).status, 403)
    // Anyone may leave
    const left = await stranger.delete(`/api/admin/posts/${postId}/editors/${stranger.id}`)
    assert.equal(left.status, 200)
    assert.equal((await stranger.get(`/api/admin/posts/${postId}`)).status, 404)
    assert.equal((await writer.delete(`/api/admin/posts/${postId}/editors/${stranger.id}`)).status, 404)
  })

  test('the admin edits every post, gives it to another owner; the owner deletes it', async () => {
    assert.equal((await admin.get(`/api/admin/posts/${postId}`)).status, 200)
    assert.equal((await admin.patch(`/api/admin/posts/${postId}`, { owner: 'nobody' })).json.error.fields.owner, 'not_found')
    const moved = await admin.patch(`/api/admin/posts/${postId}`, { owner: 'editor' })
    assert.equal(moved.status, 200)
    assert.equal(moved.json.owner.username, 'editor')

    // The one before edits it with every right; the new owner is no editor of it any more
    const editors = (await editor.get(`/api/admin/posts/${postId}/editors`)).json
    assert.deepEqual(editors.editors.map((item) => [item.user.username, item.rights]), [['writer', ['publish', 'settings', 'editors']]])
    assert.deepEqual((await writer.get(`/api/admin/posts/${postId}`)).json.rights, ['publish', 'settings', 'editors'])
    assert.equal((await writer.delete(`/api/admin/posts/${postId}`)).status, 403)

    // Taking writing away keeps what it owns and edits
    await admin.put(`/api/admin/users/${writer.id}/write`, { enabled: false })
    assert.equal((await writer.post('/api/admin/posts', draft('no-more'))).status, 403)
    assert.equal((await writer.get(`/api/admin/posts/${postId}`)).status, 200)

    assert.equal((await editor.delete(`/api/admin/posts/${postId}`)).status, 200)
    assert.equal((await writer.get(`/api/admin/posts/${postId}`)).status, 404)
  })

  test('the console: writing, editors, the owner', async () => {
    assert.match(server.console('account write stranger on'), /writing: on/)
    assert.match(server.console('post editor add by-admin stranger publish,editors'), /owner: author[^\n]*\n {2}stranger \(stranger 名\), since .+: its text, publish, editors/)
    assert.match(server.console('post editors by-admin'), /stranger/)
    assert.match(server.console('post editor add by-admin stranger delete'), /rights: invalid/)
    assert.match(server.console('post owner by-admin stranger'), /by-admin is now stranger's/)
    assert.match(server.console('post editors by-admin'), /owner: stranger[^\n]*\n {2}author \(/)
    assert.match(server.console('post editor remove by-admin author'), /No editors/)
    assert.match(server.console('account write stranger off'), /writing: off/)
  })
})

// An image as a page sends it: multipart, the post's in the path
async function uploadTo(client, postId, data) {
  const form = new FormData()
  form.append('file', new Blob([data], { type: 'image/png' }), 'a.png')
  const response = await fetch(`${baseUrl}/api/admin/posts/${postId}/uploads`, {
    method: 'POST',
    body: form,
    headers: { Origin: 'http://localhost:5173', 'X-CSRF-Token': client.csrfToken ?? '', Cookie: [...client.cookies].map(([k, v]) => `${k}=${v}`).join('; ') },
  })
  return { status: response.status, json: await response.json() }
}

describe('writers: their images, review, comments on their posts, who wrote a post', () => {
  let owner
  let helper
  let commenter
  let postId
  const png = Buffer.concat([Buffer.from('89504e470d0a1a0a', 'hex'), Buffer.alloc(2000, 1)])

  before(async () => {
    owner = await user('owner')
    helper = await user('helper')
    commenter = await user('commenter')
    await admin.put(`/api/admin/users/${owner.id}/write`, { enabled: true })
    postId = (await owner.post('/api/admin/posts', { slug: 'owned', status: 'published', originalLang: 'en', translations: { en: { title: 'Owned', body: 'Text.' } } })).json.id
    await owner.put(`/api/admin/posts/${postId}/editors`, { username: 'helper', rights: [] })
  })

  test('an invited editor knows it edits; images go to the owner\'s drive, which needs room', async () => {
    assert.equal((await helper.get('/api/auth/me')).json.user.editing, true)
    assert.equal((await commenter.get('/api/auth/me')).json.user.editing, false)

    assert.equal((await uploadTo(helper, postId, png)).json.error.code, 'owner_no_drive')
    await admin.put(`/api/admin/users/${owner.id}/drive`, { enabled: true, quota: 3000, trashDays: null })
    const first = await uploadTo(helper, postId, png)
    assert.equal(first.status, 201)
    assert.match(first.json.url, /^\/uploads\/.+\.png$/)
    // In the owner's space, not the editor's
    assert.equal((await admin.get(`/api/admin/users/${owner.id}`)).json.drive.used, png.length)
    assert.equal((await uploadTo(helper, postId, png)).json.error.code, 'owner_drive_full')
    assert.equal((await uploadTo(commenter, postId, png)).status, 404)
  })

  test('with reviews, a writer\'s publishing asks the admin, who publishes it', async () => {
    const site = (await admin.get('/api/admin/site')).json
    assert.equal(site.reviewWriters, false)
    assert.equal((await admin.put('/api/admin/site', { ...site, reviewWriters: true })).json.reviewWriters, true)

    const draft = (await owner.post('/api/admin/posts', { slug: 'reviewed', status: 'published', originalLang: 'en', translations: { en: { title: 'Read first', body: 'Text.' } } })).json
    assert.equal(draft.status, 'draft')
    assert.ok(draft.reviewRequestedAt)
    assert.equal((await owner.patch(`/api/admin/posts/${draft.id}`, { publishAt: new Date(Date.now() + 3600_000).toISOString() })).json.error.fields.publishAt, 'review_required')
    const asked = await owner.post(`/api/admin/posts/${draft.id}/publish`)
    assert.equal(asked.status, 202)
    assert.equal(asked.json.status, 'draft')

    // The admin's list of what waits for it
    assert.deepEqual((await admin.get('/api/admin/posts?status=review')).json.posts.map((post) => [post.slug, post.reviewRequested]), [['reviewed', true]])
    const published = await admin.post(`/api/admin/posts/${draft.id}/publish`)
    assert.equal(published.status, 200)
    assert.equal(published.json.status, 'published')
    assert.equal(published.json.reviewRequestedAt, null)
    assert.deepEqual((await admin.get('/api/admin/posts?status=review')).json.posts, [])

    // Asked again, then taken back
    await owner.post(`/api/admin/posts/${postId}/publish`)
    assert.ok((await owner.get(`/api/admin/posts/${postId}`)).json.reviewRequestedAt)
    assert.equal((await owner.patch(`/api/admin/posts/${postId}`, { reviewRequested: false })).json.reviewRequestedAt, null)

    // The console: what waits, declined; reviews off, the rest of the site as it was
    await owner.post(`/api/admin/posts/${postId}/publish`)
    assert.match(server.console('post list review'), /owned .* \(asks to be published\)/)
    assert.match(server.console('post decline owned'), /Declined: owned/)
    assert.match(server.console('post list review'), /No posts/)
    assert.match(server.console('site review off'), /Saved/)
    assert.deepEqual((await admin.get('/api/admin/site')).json, { ...site, reviewWriters: false })
  })

  test('the owner hides and deletes comments on its post, not on others', async () => {
    const comment = (await commenter.post('/api/comments', { target: `post:${postId}`, body: 'On the owner\'s post' })).json
    assert.equal((await helper.patch(`/api/admin/comments/${comment.id}`, { status: 'hidden' })).status, 403)
    const hidden = await owner.patch(`/api/admin/comments/${comment.id}`, { status: 'hidden' })
    assert.equal(hidden.json.status, 'hidden')
    assert.equal((await owner.delete(`/api/comments/${comment.id}`)).status, 200)

    const elsewhere = (await admin.post('/api/admin/posts', { slug: 'admins', status: 'published', originalLang: 'en', translations: { en: { title: 'Admin\'s', body: 'x' } } })).json
    const other = (await commenter.post('/api/comments', { target: `post:${elsewhere.id}`, body: 'Elsewhere' })).json
    assert.equal((await owner.patch(`/api/admin/comments/${other.id}`, { status: 'hidden' })).status, 403)
    assert.equal((await owner.delete(`/api/comments/${other.id}`)).status, 403)
  })

  test('a post names who wrote it; each has a page with what they wrote', async () => {
    const post = (await new Client().get('/api/posts/owned?lang=en')).json
    assert.deepEqual(post.people.map((person) => person.username), ['owner', 'helper'])
    for (const name of ['owner', 'helper']) {
      const page = (await new Client().get(`/api/people/${name}?lang=en`)).json
      assert.equal(page.person.displayName, `${name} 名`)
      assert.ok(page.posts.some((item) => item.slug === 'owned'))
    }
    assert.deepEqual((await new Client().get('/api/people/commenter?lang=en')).json.posts, [])
    assert.equal((await new Client().get('/api/people/nobody-at-all')).status, 404)
  })
})
