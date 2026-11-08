// Comments, moderation, bans, search, feeds and uploads against a real server (pnpm test:api).
// The test config allows 5 comments per user per post and 10 per user per minute (server.mjs).

import assert from 'node:assert/strict'
import { readdirSync, readFileSync } from 'node:fs'
import { join } from 'node:path'
import { after, before, describe, test } from 'node:test'
import { adminPassword, baseUrl, Client, lastMailTo, startServer, totp } from './server.mjs'

let server
let admin
let postId
let secondPostId

before(async () => {
  server = await startServer()
  admin = new Client()
  const login = await admin.post('/api/auth/login', { login: 'author', password: adminPassword, totp: totp(server.adminTotpSecret) })
  assert.equal(login.status, 200)

  assert.equal((await admin.post('/api/admin/tags', { slug: 'cpp', nameZh: 'C++', nameEn: 'C++' })).status, 201)
  const post = await admin.post('/api/admin/posts', {
    slug: 'websocket-in-cpp',
    status: 'published',
    originalLang: 'zh',
    tags: ['cpp'],
    translations: {
      zh: { title: '用 C++ 写 WebSocket 服务器', summary: '握手和帧', body: '在网络线程上解析握手请求，主线程处理消息。' },
      en: { title: 'A WebSocket server in C++', summary: 'Handshake and frames', body: 'Parse the handshake on the network thread.' },
    },
  })
  assert.equal(post.status, 201)
  postId = post.json.id
  secondPostId = (await admin.post('/api/admin/posts', {
    slug: 'second',
    status: 'published',
    originalLang: 'en',
    translations: { en: { title: 'Second', body: 'More text & <markup>' } },
  })).json.id
})

after(async () => {
  if (process.exitCode && server)
    console.log(server.log.join(''))
  await server?.stop()
})

async function user(name, { verify = true } = {}) {
  const client = new Client()
  const email = `${name}@example.com`
  assert.equal((await client.post('/api/auth/register', { username: name, displayName: `${name} 名`, email, password: `password ${name}` })).status, 201)
  if (verify)
    assert.equal((await client.post('/api/auth/verify-email', { token: lastMailTo(server.outbox, email).token })).status, 200)
  assert.equal((await client.post('/api/auth/login', { login: name, password: `password ${name}` })).status, 200)
  return client
}

const comments = async (client, id = postId) => (await client.get(`/api/comments?postId=${id}`)).json

describe('comments', () => {
  test('need a login with a confirmed address', async () => {
    assert.equal((await new Client().post('/api/comments', { postId, body: 'hi' })).status, 401)
    const unverified = await user('nomail', { verify: false })
    assert.equal((await unverified.post('/api/comments', { postId, body: 'hi' })).json.error.code, 'email_unverified')
  })

  test('threads with replies to replies', async () => {
    const alice = await user('alice')
    const bob = await user('bob')

    const root = await alice.post('/api/comments', { postId, body: '第一条\n两行' })
    assert.equal(root.status, 201)
    assert.equal(root.json.author.username, 'alice')
    assert.equal(root.json.parentId, null)

    const reply = await bob.post('/api/comments', { postId, body: 'reply to alice', replyTo: root.json.id })
    assert.equal(reply.json.parentId, root.json.id)
    assert.equal(reply.json.replyTo, null)

    // Replying to a reply stays in the thread and names whom it answers
    const answer = await alice.post('/api/comments', { postId, body: '@bob thanks', replyTo: reply.json.id })
    assert.equal(answer.json.parentId, root.json.id)
    assert.deepEqual(answer.json.replyTo, { id: reply.json.author.id, displayName: 'bob 名' })

    assert.equal((await bob.post('/api/comments', { postId, body: 'x', replyTo: 999999 })).json.error.fields.replyTo, 'invalid')
    assert.equal((await bob.post('/api/comments', { postId, body: '   ' })).json.error.fields.body, 'required')
    assert.equal((await bob.post('/api/comments', { postId: 999999, body: 'x' })).status, 404)

    const list = await comments(new Client())
    assert.equal(list.threads.length, 1)
    assert.equal(list.threads[0].body, '第一条\n两行')
    assert.deepEqual(list.threads[0].replies.map((r) => r.body), ['reply to alice', '@bob thanks'])

    // The post counts them
    assert.equal((await new Client().get('/api/posts/websocket-in-cpp')).json.commentCount, 3)
  })

  test('edit your own, delete keeps the thread', async () => {
    const carol = await user('carol')
    const dave = await user('dave')
    const own = await carol.post('/api/comments', { postId, body: 'typo' })
    assert.equal((await dave.patch(`/api/comments/${own.json.id}`, { body: 'not mine' })).json.error.code, 'forbidden')
    const edited = await carol.patch(`/api/comments/${own.json.id}`, { body: 'fixed' })
    assert.equal(edited.json.body, 'fixed')
    assert.ok(edited.json.editedAt)

    await dave.post('/api/comments', { postId, body: 'a reply', replyTo: own.json.id })
    assert.equal((await dave.delete(`/api/comments/${own.json.id}`)).status, 403)
    assert.equal((await carol.delete(`/api/comments/${own.json.id}`)).status, 200)

    const thread = (await comments(new Client())).threads.find((t) => t.id === own.json.id)
    assert.equal(thread.status, 'deleted')
    assert.equal(thread.body, '')
    assert.equal(thread.author, null)
    assert.equal(thread.replies.length, 1)
    assert.equal((await carol.patch(`/api/comments/${own.json.id}`, { body: 'back' })).status, 404)
  })

  test('limits per post and per minute', async () => {
    const erin = await user('erin')
    for (let i = 0; i < 5; ++i)
      assert.equal((await erin.post('/api/comments', { postId: secondPostId, body: `n${i}` })).status, 201)
    assert.equal((await erin.post('/api/comments', { postId: secondPostId, body: 'six' })).json.error.code, 'comment_limit_reached')

    // 6 counted, 4 more fit into the minute
    for (let i = 0; i < 4; ++i)
      assert.equal((await erin.post('/api/comments', { postId, body: `m${i}` })).status, 201)
    const limited = await erin.post('/api/comments', { postId, body: 'too fast' })
    assert.equal(limited.status, 429)
    assert.equal(limited.json.error.code, 'too_many_comments')
  })

  test('pages of threads', async () => {
    const frank = await user('frank')
    for (let i = 0; i < 3; ++i)
      await frank.post('/api/comments', { postId: secondPostId, body: `f${i}` })
    const page = await comments(new Client(), secondPostId)
    assert.equal(page.threads.length, 8)
    assert.equal(page.nextCursor, null)
    assert.equal(page.threads[0].body, 'f2')
  })
})

describe('moderation', () => {
  test('hidden comments are only shown to the admin', async () => {
    const gina = await user('gina')
    const comment = await gina.post('/api/comments', { postId, body: 'spam' })
    assert.equal((await gina.patch(`/api/admin/comments/${comment.json.id}`, { status: 'hidden' })).status, 403)
    assert.equal((await admin.patch(`/api/admin/comments/${comment.json.id}`, { status: 'hidden' })).json.status, 'hidden')

    assert.ok(!(await comments(new Client())).threads.some((t) => t.id === comment.json.id))
    assert.equal((await comments(admin)).threads.find((t) => t.id === comment.json.id).status, 'hidden')

    const recent = await admin.get('/api/admin/comments?status=hidden')
    assert.deepEqual(recent.json.comments.map((c) => c.body), ['spam'])
    assert.equal(recent.json.comments[0].postSlug, 'websocket-in-cpp')
  })

  test('a banned account can log in to read, not to comment', async () => {
    const harry = await user('harry')
    const id = (await harry.get('/api/auth/me')).json.user.id
    const users = await admin.get('/api/admin/users?q=harr')
    assert.deepEqual(users.json.users.map((u) => u.username), ['harry'])

    const banned = await admin.post(`/api/admin/users/${id}/ban`, { days: 7, reason: 'Spam' })
    assert.equal(banned.status, 200)
    const refused = await harry.post('/api/comments', { postId, body: 'still here' })
    assert.equal(refused.status, 403)
    assert.equal(refused.json.error.code, 'banned')
    assert.equal(refused.json.error.reason, 'Spam')
    assert.equal((await new Client().post('/api/auth/login', { login: 'harry', password: 'password harry' })).json.error.code, 'banned')

    // The admin can't be banned
    const adminId = (await admin.get('/api/auth/me')).json.user.id
    assert.equal((await admin.post(`/api/admin/users/${adminId}/ban`, {})).json.error.code, 'cannot_ban_admin')

    assert.equal((await admin.delete(`/api/admin/users/${id}/ban`)).status, 200)
    assert.equal((await harry.post('/api/comments', { postId, body: 'back' })).status, 201)
  })

  test('IP bans stop registering and commenting from an address', async () => {
    const ian = await user('ian')
    assert.equal((await admin.post('/api/admin/ip-bans', { ip: 'nonsense' })).json.error.fields.ip, 'invalid')
    const ban = await admin.post('/api/admin/ip-bans', { ip: '127.0.0.1', reason: 'wave', days: 1 })
    assert.equal(ban.status, 201)
    assert.ok(ban.json.expiresAt)
    try {
      assert.equal((await ian.post('/api/comments', { postId, body: 'x' })).json.error.code, 'ip_banned')
      const register = await new Client().post('/api/auth/register', { username: 'wave1', displayName: 'w', email: 'w@example.com', password: 'password w1' })
      assert.equal(register.json.error.code, 'ip_banned')
      assert.deepEqual((await admin.get('/api/admin/ip-bans')).json.bans.map((b) => b.ip), ['127.0.0.1'])
    } finally {
      assert.equal((await admin.delete('/api/admin/ip-bans/127.0.0.1')).status, 200)
    }
    assert.equal((await ian.post('/api/comments', { postId, body: 'x' })).status, 201)
  })
})

describe('search, feeds, uploads', () => {
  test('search finds Chinese and English text', async () => {
    const zh = await new Client().get(`/api/posts/search?q=${encodeURIComponent('网络线程')}&lang=zh`)
    assert.deepEqual(zh.json.posts.map((p) => p.slug), ['websocket-in-cpp'])
    assert.equal(zh.json.posts[0].title, '用 C++ 写 WebSocket 服务器')

    const en = await new Client().get('/api/posts/search?q=handshake&lang=en')
    assert.equal(en.json.posts[0].title, 'A WebSocket server in C++')

    // Punctuation splits words as the index does, and a tag's name or slug finds its posts
    const comma = await new Client().get(`/api/posts/search?q=${encodeURIComponent('握手请求，主线程')}&lang=zh`)
    assert.deepEqual(comma.json.posts.map((p) => p.slug), ['websocket-in-cpp'])
    assert.deepEqual((await new Client().get('/api/posts/search?q=network-thread&lang=en')).json.posts.map((p) => p.slug), ['websocket-in-cpp'])
    for (const tag of ['C++', 'c++', 'cpp'])
      assert.equal((await new Client().get(`/api/posts/search?q=${encodeURIComponent(tag)}`)).json.posts[0]?.slug, 'websocket-in-cpp', tag)
    assert.deepEqual((await new Client().get('/api/posts/search?q=%2B%2B')).json.posts, [])

    // One CJK character is enough; a word of five letters or more that is nowhere is taken as a typo
    const slugs = async (q, lang = 'en') => (await new Client().get(`/api/posts/search?lang=${lang}&q=${encodeURIComponent(q)}`)).json.posts.map((p) => p.slug)
    assert.deepEqual(await slugs('握', 'zh'), ['websocket-in-cpp'])
    assert.deepEqual(await slugs('hadnshake'), ['websocket-in-cpp'])
    assert.deepEqual(await slugs('netwrok thread'), ['websocket-in-cpp'])
    assert.deepEqual(await slugs('thraed'), ['websocket-in-cpp'])
    assert.deepEqual(await slugs('tred'), [])
    // A word is also the start of longer ones: what one is still typing
    assert.deepEqual(await slugs('hand'), ['websocket-in-cpp'])
    assert.deepEqual(await slugs('netw thr'), ['websocket-in-cpp'])
    assert.deepEqual(await slugs('handshakes'), ['websocket-in-cpp'])
    // And a part of others
    assert.deepEqual(await slugs('shake'), ['websocket-in-cpp'])
    assert.deepEqual(await slugs('ocket twor'), ['websocket-in-cpp'])
    // Its pairs are all in "handshake", not in a row
    assert.deepEqual(await slugs('shan'), [])
    assert.deepEqual(await slugs('zzzzzzz'), [])

    // The comment counts the results show follow the comments, within a second or so
    const commentCount = (await new Client().get('/api/posts/websocket-in-cpp')).json.commentCount
    let shown
    for (let tries = 0; tries < 30 && shown !== commentCount; ++tries) {
      shown = (await new Client().get('/api/posts/search?q=handshake&lang=en')).json.posts[0].commentCount
      if (shown !== commentCount)
        await new Promise((resolve) => setTimeout(resolve, 100))
    }
    assert.equal(shown, commentCount)

    assert.equal((await new Client().get('/api/posts/search?q=x')).json.error.fields.q, 'too_short')
    assert.deepEqual((await new Client().get('/api/posts/search?q=nothinglikethis')).json.posts, [])
  })

  test('RSS per language and the sitemap', async () => {
    const zh = await fetch(`${baseUrl}/api/feed.xml?lang=zh`)
    assert.match(zh.headers.get('content-type'), /application\/rss\+xml/)
    const feed = await zh.text()
    assert.match(feed, /<title>用 C\+\+ 写 WebSocket 服务器<\/title>/)
    assert.match(feed, /<link>http:\/\/localhost:5173\/zh\/posts\/websocket-in-cpp<\/link>/)
    assert.match(feed, /<category>C\+\+<\/category>/)
    assert.match(feed, /<pubDate>\w{3}, \d{2} \w{3} \d{4} \d{2}:\d{2}:\d{2} \+0000<\/pubDate>/)

    const sitemap = await (await fetch(`${baseUrl}/api/sitemap.xml`)).text()
    assert.match(sitemap, /<loc>http:\/\/localhost:5173\/en\/posts\/websocket-in-cpp<\/loc>/)
    assert.match(sitemap, /hreflang="zh" href="http:\/\/localhost:5173\/zh\/posts\/websocket-in-cpp"/)
    // "second" exists in English only
    assert.doesNotMatch(sitemap, /\/zh\/posts\/second/)
    assert.match(sitemap, /<loc>http:\/\/localhost:5173\/zh\/tags\/cpp<\/loc>/)
    assert.match(sitemap, /<loc>http:\/\/localhost:5173\/en\/sitemap<\/loc>/)

    // The crawlers find the sitemap there, and stay out of the admin's pages
    const robots = await fetch(`${baseUrl}/robots.txt`)
    assert.match(robots.headers.get('content-type'), /^text\/plain/)
    const text = await robots.text()
    assert.match(text, /^Disallow: \/zh\/admin$/m)
    assert.match(text, /^Disallow: \/en\/status$/m)
    assert.match(text, /^Sitemap: http:\/\/localhost:5173\/sitemap\.xml$/m)
  })

  test('uploads are checked by content and served back', async () => {
    const png = Buffer.concat([Buffer.from('89504e470d0a1a0a', 'hex'), Buffer.from('rest of a tiny png')])
    const upload = async (client, data, name) => {
      const form = new FormData()
      form.append('file', new Blob([data], { type: 'image/png' }), name)
      const response = await fetch(`${baseUrl}/api/admin/uploads`, {
        method: 'POST',
        body: form,
        headers: { Origin: 'http://localhost:5173', 'X-CSRF-Token': client.csrfToken ?? '', Cookie: [...client.cookies].map(([k, v]) => `${k}=${v}`).join('; ') },
      })
      return { status: response.status, json: await response.json() }
    }

    const reader = await user('jane')
    assert.equal((await upload(reader, png, 'a.png')).status, 403)

    const fake = await upload(admin, Buffer.from('<script>alert(1)</script>'), 'evil.png')
    assert.equal(fake.json.error.fields.file, 'unsupported_type')

    const ok = await upload(admin, png, '../../etc/passwd.png')
    assert.equal(ok.status, 201)
    assert.match(ok.json.url, /^\/uploads\/[A-Za-z0-9_-]{22}\.png$/)

    const served = await fetch(`${baseUrl}${ok.json.url}`)
    assert.equal(served.headers.get('content-type'), 'image/png')
    assert.deepEqual(Buffer.from(await served.arrayBuffer()), png)
    assert.equal((await fetch(`${baseUrl}/uploads/..%2F..%2Fetc%2Fpasswd.png`)).status, 400)
    assert.equal((await fetch(`${baseUrl}/uploads/nothere.png`)).status, 404)
  })
})

describe('rebuilding the prerendered pages', () => {
  test('runs once after a burst of changes to published posts', async () => {
    const { existsSync, readFileSync } = await import('node:fs')
    const count = () => (existsSync(server.rebuildsFile) ? readFileSync(server.rebuildsFile, 'utf8').split('\n').filter(Boolean).length : 0)

    // Whatever earlier tests triggered has run by now (debounce 1 s)
    await new Promise((r) => setTimeout(r, 1500))
    const before = count()

    await admin.patch(`/api/admin/posts/${secondPostId}`, { translations: { en: { title: 'Second, edited', body: 'x' } } })
    await admin.patch(`/api/admin/posts/${secondPostId}`, { translations: { en: { title: 'Second, edited again', body: 'x' } } })
    // Drafts don't matter to the public pages
    await admin.post('/api/admin/posts', { slug: 'a-draft', originalLang: 'en', translations: { en: { title: 'Draft', body: 'x' } } })

    await new Promise((r) => setTimeout(r, 600))
    assert.equal(count(), before)
    await new Promise((r) => setTimeout(r, 1500))
    assert.equal(count(), before + 1)
  })
})

describe('reply mails', () => {
  test("a reply mails the author of the comment it answers once they turned it on, off by default", async () => {
    const mailsTo = (address) => readdirSync(server.outbox).filter((name) => readFileSync(join(server.outbox, name), 'utf8').includes(`To: <${address}>`)).length
    const zh = (await admin.post('/api/admin/posts', {
      slug: 'reply-mails', status: 'published', originalLang: 'zh', translations: { zh: { title: '回复提醒', body: '正文' } },
    })).json.id
    const en = (await admin.post('/api/admin/posts', {
      slug: 'reply-mails-en', status: 'published', originalLang: 'en', translations: { en: { title: 'Replies', body: 'Text' } },
    })).json.id
    const quill = await user('quill')
    const rook = await user('rook')
    const root = (await quill.post('/api/comments', { postId: zh, body: '有人吗' })).json
    const before = mailsTo('quill@example.com')

    // Off until the account turns it on
    assert.equal((await quill.get('/api/auth/me')).json.user.replyMails, false)
    await rook.post('/api/comments', { postId: zh, body: '还没开', replyTo: root.id })
    assert.equal(mailsTo('quill@example.com'), before)
    for (const client of [quill, rook])
      assert.equal((await client.patch('/api/auth/me', { replyMails: true })).json.user.replyMails, true)

    const reply = (await rook.post('/api/comments', { postId: zh, body: '有的\n第二行', replyTo: root.id })).json
    assert.equal(mailsTo('quill@example.com'), before + 1)
    const mail = lastMailTo(server.outbox, 'quill@example.com').text
    assert.match(mail, /^quill 名，你好：/)
    assert.match(mail, /rook 名 在《回复提醒》下回复了你的评论：\n\n> 有的\n> 第二行\n/)
    assert.ok(mail.includes(`/zh/posts/reply-mails#comment-${reply.id}`))
    assert.ok(mail.includes('/zh/account'))
    // Nobody hears of their own replies; the author of the reply answered does
    await quill.post('/api/comments', { postId: zh, body: '自己回自己', replyTo: root.id })
    assert.equal(mailsTo('quill@example.com'), before + 1)
    await quill.post('/api/comments', { postId: zh, body: '@rook 谢谢', replyTo: reply.id })
    assert.match(lastMailTo(server.outbox, 'rook@example.com').text, /quill 名 在《回复提醒》下回复了你的评论/)

    // Turned off
    const off = await quill.patch('/api/auth/me', { replyMails: false })
    assert.equal(off.json.user.replyMails, false)
    await rook.post('/api/comments', { postId: zh, body: 'again', replyTo: root.id })
    assert.equal(mailsTo('quill@example.com'), before + 1)

    // In the reader's language when the page has it; an English post only has the English page
    await quill.patch('/api/auth/me', { replyMails: true, locale: 'en' })
    const english = (await quill.post('/api/comments', { postId: en, body: 'hello' })).json
    await rook.post('/api/comments', { postId: en, body: 'hi quill', replyTo: english.id })
    const mailEn = lastMailTo(server.outbox, 'quill@example.com').text
    assert.match(mailEn, /^Hi quill 名,/)
    assert.match(mailEn, /rook 名 replied to your comment under “Replies”:\n\n> hi quill\n/)
    assert.ok(mailEn.includes('/en/posts/reply-mails-en#comment-'))
  })
})

describe('comments on passages', () => {
  const block = '主线程从不阻塞，慢的事交给工作线程。'
  const passage = (quote, extra = {}) => {
    const at = block.indexOf(quote)
    return { lang: 'zh', block: 3, quote, prefix: block.slice(Math.max(0, at - 32), at), suffix: block.slice(at + quote.length, at + quote.length + 32), text: block, ...extra }
  }

  test('a passage, replies that keep it or choose their own, the threads of a language', async () => {
    const id = (await admin.post('/api/admin/posts', {
      slug: 'passages', status: 'published', originalLang: 'zh', translations: { zh: { title: '段落', body: block } },
    })).json.id
    const target = `post:${id}`
    const asker = await user('pasker')
    const helper = await user('phelper')

    const root = await asker.post('/api/comments', { target, body: '为什么不能阻塞？', anchor: passage('从不阻塞') })
    assert.equal(root.status, 201, JSON.stringify(root.json))
    // Its block's text only once the post lost it
    assert.deepEqual(root.json.anchor, { lang: 'zh', block: 3, quote: '从不阻塞', prefix: '主线程', suffix: '，慢的事交给工作线程。', text: null, status: 'ok' })
    assert.equal(root.json.replyToId, null)

    // A reply keeps the passage of what it answers (none of its own); one may choose another
    const kept = (await helper.post('/api/comments', { target, body: '因为……', replyTo: root.json.id })).json
    assert.equal(kept.anchor, null)
    assert.equal(kept.replyToId, root.json.id)
    const own = (await asker.post('/api/comments', { target, body: '那这里呢？', replyTo: kept.id, anchor: passage('工作线程') })).json
    assert.equal(own.anchor.quote, '工作线程')
    assert.equal(own.replyToId, kept.id)
    assert.equal(own.parentId, root.json.id)

    const anchored = (await new Client().get(`/api/comments/anchored?target=${target}&lang=zh`)).json.threads
    assert.deepEqual(anchored.map((thread) => thread.id), [root.json.id])
    assert.deepEqual(anchored[0].replies.map((reply) => [reply.id, reply.anchor?.quote ?? null]), [[kept.id, null], [own.id, '工作线程']])
    // Not in the other language; the comment area has it with the others
    assert.deepEqual((await new Client().get(`/api/comments/anchored?target=${target}&lang=en`)).json.threads, [])
    assert.equal((await new Client().get(`/api/comments?target=${target}`)).json.threads[0].anchor.quote, '从不阻塞')
  })

  test('what a passage may be', async () => {
    const id = (await admin.post('/api/admin/posts', {
      slug: 'passages-checked', status: 'published', originalLang: 'zh', translations: { zh: { title: '检查', body: block } },
    })).json.id
    const client = await user('pchecker')
    const send = (body) => client.post('/api/comments', { target: `post:${id}`, body: 'x', ...body })
    assert.deepEqual((await client.post('/api/comments', { target: 'guestbook', body: 'x', anchor: passage('从不阻塞') })).json.error.fields, { anchor: 'not_allowed' })
    assert.deepEqual((await send({ anchor: passage('从不阻塞', { quote: '不在这一段' }) })).json.error.fields, { 'anchor.quote': 'not_in_text' })
    assert.deepEqual((await send({ anchor: passage('从不阻塞', { lang: 'en' }) })).json.error.fields, { 'anchor.lang': 'invalid' })
    assert.deepEqual((await send({ anchor: passage('从不阻塞', { quote: '长'.repeat(301), text: '长'.repeat(301) }) })).json.error.fields, { 'anchor.quote': 'too_long' })
    // Taken as sent: spaces around are the text around it
    const spaced = await send({ anchor: passage('从不阻塞', { prefix: ' 主线程 ', text: block + ' ' }) })
    assert.equal(spaced.json.anchor.prefix, ' 主线程 ')
  })

  test('choosing the passage again: its author, a moderator, nobody else', async () => {
    const id = (await admin.post('/api/admin/posts', {
      slug: 'passages-moved', status: 'published', originalLang: 'zh', translations: { zh: { title: '重选', body: block } },
    })).json.id
    const author = await user('pmover')
    const other = await user('pother')
    const comment = (await author.post('/api/comments', { target: `post:${id}`, body: '问题', anchor: passage('从不阻塞') })).json

    const moved = await author.patch(`/api/comments/${comment.id}`, { anchor: passage('工作线程') })
    assert.equal(moved.status, 200, JSON.stringify(moved.json))
    assert.equal(moved.json.anchor.quote, '工作线程')
    assert.equal(moved.json.body, '问题')
    assert.equal((await other.patch(`/api/comments/${comment.id}`, { anchor: passage('主线程') })).status, 403)
    const byAdmin = await admin.patch(`/api/comments/${comment.id}`, { anchor: passage('主线程') })
    assert.equal(byAdmin.status, 200, JSON.stringify(byAdmin.json))
    assert.equal(byAdmin.json.anchor.quote, '主线程')
    // A moderator chooses its passage, never its text
    assert.equal((await admin.patch(`/api/comments/${comment.id}`, { body: '改别人的话' })).status, 403)
  })
})

describe('a post that changes under its passages', () => {
  test('found again through its Markdown, lost, kept, found again; its versions', async () => {
    const body = (middle) => `开头一段。\n\n这里有**加粗的话**和[一个链接](https://example.com/x)。${middle}\n\n结尾一段。`
    const created = (await admin.post('/api/admin/posts', {
      slug: 'changing', status: 'published', originalLang: 'zh', translations: { zh: { title: '会变', body: body('还有一句。') } },
    })).json
    const reader = await user('pchanging')
    // As the page shows it: no Markdown
    const text = '这里有加粗的话和一个链接。还有一句。'
    const comment = (await reader.post('/api/comments', {
      target: `post:${created.id}`, body: '这句什么意思？', anchor: { lang: 'zh', block: 1, quote: '加粗的话和一个链接', prefix: '这里有', suffix: '。还有一句。', text },
    })).json
    const edit = (middle) => admin.patch(`/api/admin/posts/${created.id}`, { translations: { zh: { title: '会变', body: body(middle) } } })
    const anchorNow = async () => (await new Client().get(`/api/comments/anchored?target=post:${created.id}&lang=zh`)).json.threads[0].anchor
    const orphaned = async () => (await admin.get('/api/admin/comments?passage=orphaned')).json.comments.map((c) => c.id)

    // Another sentence changes: still there
    assert.equal((await edit('换了一句。')).status, 200)
    assert.equal((await anchorNow()).status, 'ok')

    // Its sentence goes: orphaned, its block as it was shown, the admin's to decide
    await admin.patch(`/api/admin/posts/${created.id}`, { translations: { zh: { title: '会变', body: '开头一段。\n\n全改了。\n\n结尾一段。' } } })
    assert.deepEqual(await anchorNow(), { ...comment.anchor, text, status: 'orphaned' })
    assert.ok((await orphaned()).includes(comment.id))
    const listed = (await admin.get('/api/admin/comments?passage=orphaned')).json.comments.find((c) => c.id === comment.id)
    assert.deepEqual(listed.passage, { lang: 'zh', quote: '加粗的话和一个链接', text, status: 'orphaned' })

    // Kept: still under "the text changed", no longer to decide
    const kept = await admin.patch(`/api/admin/comments/${comment.id}`, { passage: 'kept' })
    assert.equal(kept.status, 200, JSON.stringify(kept.json))
    assert.equal(kept.json.passage.status, 'kept')
    assert.equal(kept.json.status, 'visible')
    assert.ok(!(await orphaned()).includes(comment.id))
    assert.equal((await anchorNow()).status, 'kept')

    // Its text back: ok again
    await edit('又回来了。')
    assert.equal((await anchorNow()).status, 'ok')

    // 4 versions published (made, 3 edits), every one kept, newest first; one in full with the one before it
    const revisions = (await admin.get(`/api/admin/posts/${created.id}/revisions`)).json.revisions
    assert.equal(revisions.length, 4)
    assert.deepEqual(new Set(revisions.map((r) => [r.lang, r.kind].join())), new Set(['zh,published']))
    assert.ok(revisions[0].id > revisions[1].id)
    const newest = (await admin.get(`/api/admin/posts/${created.id}/revisions/${revisions[0].id}`)).json
    assert.ok(newest.body.includes('又回来了'))
    assert.ok(newest.previous.body.includes('全改了'))

    // A draft's changes make no versions
    await admin.patch(`/api/admin/posts/${created.id}`, { status: 'draft' })
    await edit('草稿里改的。')
    assert.equal((await admin.get(`/api/admin/posts/${created.id}/revisions`)).json.revisions[0].id, revisions[0].id)
  })
})
