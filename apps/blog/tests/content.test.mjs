// Notes, thinking, the site's profile, the timeline, friends' links and projects, and comments on
// all of them, against a real server (pnpm test:api).

import assert from 'node:assert/strict'
import { after, before, describe, test } from 'node:test'
import { adminPassword, Client, lastMailTo, startServer, totp } from './server.mjs'

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
  return client
}

const note = (extra = {}) => ({
  status: 'published',
  originalLang: 'zh',
  mood: '开心',
  weather: '晴',
  translations: { zh: { title: '今天', body: '和猫一起看星星。' } },
  ...extra,
})

describe('notes', () => {
  let first
  let second

  test('create, validate, read by number', async () => {
    const invalid = await admin.post('/api/admin/notes', { originalLang: 'en', mood: 'x'.repeat(21), translations: { zh: { title: '' } } })
    assert.equal(invalid.status, 400)
    assert.deepEqual(invalid.json.error.fields, {
      mood: 'too_long', 'translations.zh.title': 'required', 'translations.zh.body': 'required', 'translations.en': 'required',
    })
    assert.equal((await new Client().post('/api/admin/notes', note())).status, 401)

    first = (await admin.post('/api/admin/notes', note())).json
    assert.equal(first.status, 'published')
    assert.ok(first.publishedAt)
    second = (await admin.post('/api/admin/notes', note({
      translations: { zh: { title: '第二篇', body: '星空' }, en: { title: 'Second', body: 'Stars' } },
    }))).json
    assert.ok(second.id > first.id)

    const zh = (await new Client().get(`/api/notes/${first.id}?lang=zh`)).json
    assert.equal(zh.title, '今天')
    assert.equal(zh.mood, '开心')
    assert.equal(zh.weather, '晴')
    // 今天 and 和猫一起看星星, the full stop isn't a word
    assert.equal(zh.wordCount, 9)
    assert.equal(zh.previous, null)
    assert.deepEqual(zh.next, { id: second.id, title: '第二篇' })

    // No English version: not on the English site, neither itself nor as a neighbour
    const en = await new Client().get(`/api/notes/${first.id}?lang=en`)
    assert.equal(en.status, 404)
    assert.deepEqual(en.json.error, { code: 'not_in_lang', translations: ['zh'] })
    assert.equal((await new Client().get(`/api/notes/${second.id}?lang=en`)).json.previous, null)
  })

  test('drafts stay private, lists page by cursor', async () => {
    const draft = (await admin.post('/api/admin/notes', note({ status: 'draft' }))).json
    assert.equal((await new Client().get(`/api/notes/${draft.id}`)).status, 404)
    assert.equal((await admin.get(`/api/admin/notes/${draft.id}`)).json.status, 'draft')
    assert.ok((await admin.get('/api/admin/notes?status=draft')).json.notes.some((n) => n.id === draft.id))

    const page = (await new Client().get('/api/notes?lang=zh&limit=1')).json
    assert.equal(page.notes.length, 1)
    assert.equal(page.notes[0].id, second.id)
    assert.equal(page.notes[0].title, '第二篇')
    assert.equal(page.notes[0].excerpt, '星空')
    const next = (await new Client().get(`/api/notes?lang=zh&limit=1&cursor=${page.nextCursor}`)).json
    assert.deepEqual(next.notes.map((n) => n.id), [first.id])
    assert.equal(next.nextCursor, null)
    // In English only the one written in it
    assert.deepEqual((await new Client().get('/api/notes?lang=en')).json.notes.map((n) => [n.id, n.title]), [[second.id, 'Second']])

    assert.deepEqual((await new Client().get('/api/posts/slugs')).json.notes.find((n) => n.id === first.id), { id: first.id, translations: ['zh'] })
  })

  test('update keeps the original language, delete', async () => {
    const missing = await admin.patch(`/api/admin/notes/${second.id}`, { translations: { zh: null } })
    assert.deepEqual(missing.json.error.fields, { 'translations.zh': 'required' })
    const updated = await admin.patch(`/api/admin/notes/${second.id}`, { weather: '雨', translations: { en: null } })
    assert.equal(updated.json.weather, '雨')
    assert.deepEqual(Object.keys(updated.json.translations), ['zh'])

    const temporary = (await admin.post('/api/admin/notes', note())).json
    assert.equal((await admin.delete(`/api/admin/notes/${temporary.id}`)).status, 200)
    assert.equal((await admin.get(`/api/admin/notes/${temporary.id}`)).status, 404)
  })
})

describe('thinking', () => {
  test('a one-liner list in the reader language', async () => {
    const created = await admin.post('/api/admin/thinking', { originalLang: 'en', translations: { en: { body: 'Cats know.' }, zh: { body: '猫知道。' } } })
    assert.equal(created.status, 201)
    assert.equal(created.json.status, 'published')
    await admin.post('/api/admin/thinking', { originalLang: 'zh', status: 'draft', translations: { zh: { body: '还没想好' } } })

    const list = (await new Client().get('/api/thinking?lang=zh')).json
    assert.deepEqual(list.thinking.map((t) => t.body), ['猫知道。'])
    assert.equal(list.thinking[0].fallback, false)
    assert.equal((await admin.get('/api/admin/thinking')).json.thinking.length, 2)

    assert.equal((await admin.patch(`/api/admin/thinking/${created.json.id}`, { translations: { zh: { body: '猫什么都知道。' } } })).status, 200)
    assert.equal((await new Client().get('/api/thinking?lang=zh')).json.thinking[0].body, '猫什么都知道。')
    assert.equal((await admin.post('/api/admin/thinking', { originalLang: 'zh', translations: { zh: { body: 'x'.repeat(2001) } } })).json.error.fields['translations.zh.body'], 'too_long')
  })
})

describe('comments everywhere', () => {
  test('on a note, a thinking and the guestbook', async () => {
    const reader = await user('stargazer')
    const noteId = (await admin.post('/api/admin/notes', note())).json.id
    const thinkingId = (await admin.post('/api/admin/thinking', { originalLang: 'zh', translations: { zh: { body: '喵' } } })).json.id

    for (const target of [`note:${noteId}`, `thinking:${thinkingId}`, 'guestbook']) {
      const comment = await reader.post('/api/comments', { target, body: `on ${target}` })
      assert.equal(comment.status, 201, JSON.stringify(comment.json))
      assert.equal(comment.json.target, target)
      const reply = await admin.post('/api/comments', { target, body: 'thanks', replyTo: comment.json.id })
      assert.equal(reply.status, 201)

      const threads = (await new Client().get(`/api/comments?target=${target}`)).json.threads
      assert.equal(threads.length, 1)
      assert.equal(threads[0].body, `on ${target}`)
      assert.equal(threads[0].replies.length, 1)
    }

    // A reply has to stay on its target
    const guestbook = (await new Client().get('/api/comments?target=guestbook')).json.threads[0]
    const crossed = await reader.post('/api/comments', { target: `note:${noteId}`, body: 'x', replyTo: guestbook.id })
    assert.deepEqual(crossed.json.error.fields, { replyTo: 'invalid' })

    assert.equal((await reader.post('/api/comments', { target: 'note:999999', body: 'x' })).status, 404)
    assert.equal((await reader.post('/api/comments', { target: 'nothing:1', body: 'x' })).json.error.fields.target, 'required')
    assert.equal((await new Client().get('/api/comments?target=bad')).status, 400)

    // Deleting the note deletes its comments
    assert.equal((await admin.delete(`/api/admin/notes/${noteId}`)).status, 200)
    assert.ok(!(await admin.get('/api/admin/comments')).json.comments.some((c) => c.target === `note:${noteId}`))
    assert.ok((await admin.get('/api/admin/comments')).json.comments.some((c) => c.target === 'guestbook'))
  })
})

describe('the site', () => {
  test('the console shows and changes the profile', async () => {
    const before = JSON.parse(server.console('site export').trim().split('\n').at(-1))

    assert.match(server.console('site set since 2026-02-30'), /Nothing was saved:\n {2}since: invalid/)
    assert.match(server.console('site set quote zh 人间有味是清欢'), /Saved/)
    assert.match(server.console('site set nothing zh x'), /Syntax: site set/)
    assert.equal((await new Client().get('/api/site?lang=zh')).json.quote, '人间有味是清欢')

    const profile = { ...before, about: { zh: '# 关于我\n\n第二行', en: '' }, socials: [{ kind: 'github', url: 'https://github.com/example' }] }
    assert.match(server.console(`site import ${JSON.stringify(profile)}`), /Saved/)
    assert.equal((await new Client().get('/api/site/about?lang=zh')).json.about, '# 关于我\n\n第二行')
    assert.match(server.console('site import {"socials": [{"kind": "myspace", "url": "x"}]}'), /socials\.0\.kind: invalid/)
    assert.match(server.console('site show'), /about +zh +# 关于我 \.\.\. \(10 characters\)\nabout +en +\n/)

    // As it was, for the next test
    assert.match(server.console(`site import ${JSON.stringify(before)}`), /Saved/)
  })

  test('profile in both languages, stats', async () => {
    const invalid = await admin.put('/api/admin/site', { since: '2026-13-01', avatar: 'javascript:x', socials: [{ kind: 'myspace', url: 'x' }] })
    assert.equal(invalid.status, 400)
    assert.deepEqual(invalid.json.error.fields, { since: 'invalid', avatar: 'invalid', 'socials.0.kind': 'invalid', 'socials.0.url': 'invalid' })

    const saved = await admin.put('/api/admin/site', {
      name: { zh: '黑猫与星空', en: '' },
      author: 'Aiden',
      tagline: { zh: '在屋顶上写 **C++**', en: 'Writing **C++** on the roof' },
      bio: { zh: '写服务器的', en: '' },
      since: '2026-10-01',
      socials: [{ kind: 'github', url: 'https://github.com/example' }, { kind: 'email', url: 'mailto:me@example.com' }],
      about: { zh: '# 关于我', en: '# About me' },
    })
    assert.equal(saved.status, 200)
    assert.equal(saved.json.name.en, '')

    const en = (await new Client().get('/api/site?lang=en')).json
    // Without an English name or bio: the Chinese ones
    assert.equal(en.name, '黑猫与星空')
    assert.equal(en.bio, '写服务器的')
    assert.equal(en.tagline, 'Writing **C++** on the roof')
    assert.equal(en.since, '2026-10-01T00:00:00Z')
    assert.equal(en.socials.length, 2)
    // Days in UTC, counting today
    assert.equal(en.stats.days, Math.floor((Date.now() - Date.UTC(2026, 9, 1)) / 86400000) + 1)
    assert.ok(en.stats.notes >= 2)
    assert.ok(en.stats.words > 0)
    assert.equal((await new Client().get('/api/site/about?lang=en')).json.about, '# About me')

    // The feeds take the name from the profile
    const feed = await fetch(`${new URL('/api/feed.xml?lang=zh', (await import('./server.mjs')).baseUrl)}`)
    const xml = await feed.text()
    assert.match(xml, /<title>黑猫与星空<\/title>/)
    // Notes are in it too
    assert.match(xml, /<link>http:\/\/localhost:5173\/zh\/notes\/\d+<\/link>/)
  })

  test('timeline of everything published', async () => {
    await admin.post('/api/admin/posts', { slug: 'timeline-post', status: 'published', originalLang: 'en', translations: { en: { title: 'A post', body: 'Text' } } })
    await admin.post('/api/admin/notes', note({ originalLang: 'en', translations: { en: { title: 'A note', body: 'Text' } } }))
    // On the English site: not on the Chinese one
    assert.ok(!(await new Client().get('/api/timeline?lang=zh')).json.items.some((i) => i.slug === 'timeline-post'))
    const items = (await new Client().get('/api/timeline?lang=en')).json.items
    // Times have a second's resolution: what was made in the same second comes in any order
    assert.ok(items.some((i) => i.kind === 'post' && i.slug === 'timeline-post' && i.title === 'A post'))
    assert.ok(items.some((i) => i.kind === 'note'))
    assert.ok(items.some((i) => i.kind === 'thinking'))
    for (let i = 1; i < items.length; ++i)
      assert.ok(items[i - 1].publishedAt >= items[i].publishedAt)
  })

  test('friends apply, the author reviews', async () => {
    const reader = await user('neighbour')
    const application = { name: '邻居', url: 'https://neighbour.example.com', description: '隔壁的猫', message: '互链吗' }
    assert.equal((await new Client().post('/api/friends', application)).status, 401)
    assert.equal((await reader.post('/api/friends', { ...application, url: 'ftp://x' })).json.error.fields.url, 'invalid')
    const applied = await reader.post('/api/friends', application)
    assert.equal(applied.status, 201)
    assert.equal(applied.json.status, 'pending')
    assert.equal((await reader.post('/api/friends', application)).status, 409)
    assert.deepEqual((await new Client().get('/api/friends')).json.friends, [])

    const pending = (await admin.get('/api/admin/friends?status=pending')).json.friends
    assert.equal(pending[0].message, '互链吗')
    assert.equal(pending[0].applicant.username, 'neighbour')

    const approved = await admin.patch(`/api/admin/friends/${applied.json.id}`, { status: 'approved' })
    assert.equal(approved.json.status, 'approved')
    assert.match(lastMailTo(server.outbox, 'neighbour@example.com').text, /已经出现在黑猫与星空的友链里/)
    assert.deepEqual((await new Client().get('/api/friends')).json.friends.map((f) => f.name), ['邻居'])

    // Another one, rejected with a reason
    const other = (await reader.post('/api/friends', { name: 'Spam', url: 'https://spam.example.com' })).json
    await admin.patch(`/api/admin/friends/${other.id}`, { status: 'rejected', rejectReason: '不相关' })
    assert.match(lastMailTo(server.outbox, 'neighbour@example.com').text, /原因：不相关/)

    // Added by the author directly
    const own = await admin.post('/api/admin/friends', { name: 'Mine', url: 'https://mine.example.com', sort: -1 })
    assert.equal(own.status, 201)
    assert.deepEqual((await new Client().get('/api/friends')).json.friends.map((f) => f.name), ['Mine', '邻居'])
    assert.equal((await admin.delete(`/api/admin/friends/${own.json.id}`)).status, 200)
  })

  test('projects', async () => {
    const invalid = await admin.post('/api/admin/projects', { name: '', repoUrl: 'nope' })
    assert.deepEqual(invalid.json.error.fields, { name: 'required', repoUrl: 'invalid' })
    const b = (await admin.post('/api/admin/projects', { name: 'B', sort: 2, descriptionZh: '乙' })).json
    const a = (await admin.post('/api/admin/projects', { name: 'Rooftree', repoUrl: 'https://github.com/example/Rooftree', sort: 1 })).json
    assert.deepEqual((await new Client().get('/api/projects')).json.projects.map((p) => p.name), ['Rooftree', 'B'])
    assert.equal((await admin.patch(`/api/admin/projects/${b.id}`, { name: 'B2', sort: 0 })).json.name, 'B2')
    assert.deepEqual((await new Client().get('/api/projects')).json.projects.map((p) => p.id), [b.id, a.id])
    assert.equal((await admin.delete(`/api/admin/projects/${a.id}`)).status, 200)
  })

  test('uploads keep their placeholder', async () => {
    // 1x1 PNG
    const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==', 'base64')
    const upload = async (fields) => {
      const form = new FormData()
      form.set('file', new Blob([png], { type: 'image/png' }), 'a.png')
      for (const [key, value] of Object.entries(fields)) form.set(key, value)
      const response = await fetch(new URL('/api/admin/uploads', (await import('./server.mjs')).baseUrl), {
        method: 'POST',
        body: form,
        headers: { Origin: (await import('./server.mjs')).origin, 'X-CSRF-Token': admin.csrfToken, Cookie: [...admin.cookies].map(([k, v]) => `${k}=${v}`).join('; ') },
      })
      return { status: response.status, json: await response.json() }
    }

    assert.equal((await upload({ width: '1', thumbhash: 'bad hash!' })).status, 400)
    const uploaded = await upload({ width: '640', height: '480', thumbhash: 'YJqGPQw7sFlslqhFafSE+Q6oJ1h2iHB2Rw==' })
    assert.equal(uploaded.status, 201)
    const name = uploaded.json.url.split('/').pop()
    const meta = (await new Client().get('/api/uploads/meta')).json.images
    assert.deepEqual(meta[name], { thumbhash: 'YJqGPQw7sFlslqhFafSE+Q6oJ1h2iHB2Rw==', width: 640, height: 480 })
  })
})

describe('search', () => {
  test('finds notes too', async () => {
    await admin.post('/api/admin/notes', note({ translations: { zh: { title: '流星雨之夜', body: '看到了很多流星。' } } }))
    const found = (await new Client().get(`/api/posts/search?lang=zh&q=${encodeURIComponent('流星')}`)).json
    assert.deepEqual(found.notes.map((n) => n.title), ['流星雨之夜'])
    assert.equal(found.posts.length, 0)
  })

  test('finds what was published as soon as it answered, never what was deleted', async () => {
    const search = async (q) => (await new Client().get(`/api/posts/search?lang=en&q=${encodeURIComponent(q)}`)).json.posts.map((p) => p.slug)
    const created = await admin.post('/api/admin/posts', {
      slug: 'zebra-crossing', status: 'published', originalLang: 'en', translations: { en: { title: 'Zebra crossing', body: 'Stripes on the road.' } },
    })
    assert.deepEqual(await search('zebra'), ['zebra-crossing'])
    await admin.patch(`/api/admin/posts/${created.json.id}`, { status: 'draft' })
    assert.deepEqual(await search('zebra'), [])
    await admin.patch(`/api/admin/posts/${created.json.id}`, { status: 'published' })
    assert.deepEqual(await search('stripes'), ['zebra-crossing'])
    assert.equal((await admin.delete(`/api/admin/posts/${created.json.id}`)).status, 200)
    assert.deepEqual(await search('stripes'), [])
  })

  test('searches the page language, or with all=1 every language in the version found', async () => {
    const search = async (q, query) => (await new Client().get(`/api/posts/search?q=${encodeURIComponent(q)}&${query}`)).json.posts
    const created = await admin.post('/api/admin/posts', {
      slug: 'okapi', status: 'published', originalLang: 'zh',
      translations: { zh: { title: '霍加狓', body: '森林里的长颈鹿。' }, en: { title: 'Okapi', body: 'A forest giraffe.' } },
    })
    const english = await admin.post('/api/admin/posts', {
      slug: 'wombat', status: 'published', originalLang: 'en', translations: { en: { title: 'Wombat', body: 'Cubes.' } },
    })
    try {
      assert.deepEqual(await search('okapi', 'lang=zh'), [])
      assert.deepEqual(await search('wombat', 'lang=zh'), [])
      assert.deepEqual((await search('长颈鹿', 'lang=zh')).map((p) => p.title), ['霍加狓'])
      assert.deepEqual(await search('长颈鹿', 'lang=en'), [])

      const okapi = await search('okapi', 'lang=zh&all=1')
      assert.deepEqual(okapi.map((p) => [p.title, p.lang, p.fallback]), [['Okapi', 'en', true]])
      const wombat = await search('wombat', 'lang=zh&all=1')
      assert.deepEqual(wombat.map((p) => [p.title, p.lang]), [['Wombat', 'en']])
      assert.deepEqual((await search('长颈鹿', 'lang=en&all=1')).map((p) => p.title), ['霍加狓'])
    } finally {
      await admin.delete(`/api/admin/posts/${created.json.id}`)
      await admin.delete(`/api/admin/posts/${english.json.id}`)
    }
  })
})

describe('posts', () => {
  test('previous and next', async () => {
    const make = (slug) => admin.post('/api/admin/posts', { slug, status: 'published', originalLang: 'zh', translations: { zh: { title: slug, body: '正文' } } })
    await make('older')
    await make('newer')
    const older = (await new Client().get('/api/posts/older?lang=zh')).json
    assert.deepEqual(older.next, { slug: 'newer', title: 'newer' })
    const newer = (await new Client().get('/api/posts/newer?lang=zh')).json
    assert.equal(newer.next, null)
    assert.deepEqual(newer.previous, { slug: 'older', title: 'older' })
  })

  // The lists, the pages and the site's counts are kept in memory (server/ReadCache.h): every change
  // they show has to drop them, else a reader sees what was
  test('what is kept in memory follows every change it shows', async () => {
    const reader = new Client()
    const read = async () => {
      const list = (await reader.get('/api/posts?lang=en')).json
      return {
        listed: list.posts.find((p) => p.slug === 'kept-post'),
        slugs: list.posts.map((p) => p.slug),
        post: (await reader.get('/api/posts/kept-post?lang=en')).json,
        site: (await reader.get('/api/site?lang=en')).json,
      }
    }

    // Before what is read: an account's change drops it all too, which would hide a missing drop
    const commenter = await user('keptreader')
    assert.equal((await admin.post('/api/admin/tags', { slug: 'kept', nameZh: '缓存', nameEn: 'Kept' })).status, 201)
    const created = await admin.post('/api/admin/posts', {
      slug: 'kept-post', status: 'published', originalLang: 'en', tags: ['kept'], translations: { en: { title: 'Kept', body: 'Text' } },
    })
    assert.equal(created.status, 201)
    await read()
    // Now from memory
    let now = await read()
    assert.equal(now.post.title, 'Kept')
    assert.equal(now.listed.commentCount, 0)
    const posts = now.site.stats.posts

    assert.equal((await commenter.post('/api/comments', { target: `post:${created.json.id}`, body: 'Hello' })).status, 201)
    now = await read()
    assert.equal(now.post.commentCount, 1)
    assert.equal(now.listed.commentCount, 1)

    assert.equal((await admin.patch(`/api/admin/posts/${created.json.id}`, { translations: { en: { title: 'Changed', body: 'Text' } } })).status, 200)
    now = await read()
    assert.equal(now.post.title, 'Changed')
    assert.equal(now.listed.title, 'Changed')

    assert.equal((await admin.patch('/api/admin/tags/kept', { slug: 'kept', nameZh: '缓存', nameEn: 'Memory' })).status, 200)
    now = await read()
    assert.deepEqual(now.post.tags, [{ slug: 'kept', name: 'Memory' }])

    const me = (await admin.get('/api/auth/me')).json.user
    assert.equal((await admin.patch('/api/auth/me', { displayName: 'Renamed Author', locale: me.locale })).status, 200)
    now = await read()
    assert.equal(now.post.author, 'Renamed Author')
    assert.equal((await admin.patch('/api/auth/me', { displayName: me.displayName, locale: me.locale })).status, 200)

    await admin.post('/api/admin/posts', { slug: 'kept-second', status: 'published', originalLang: 'en', translations: { en: { title: 'Second', body: 'Text' } } })
    now = await read()
    assert.equal(now.site.stats.posts, posts + 1)
    assert.equal(now.slugs[0], 'kept-second')
    assert.deepEqual(now.post.next, { slug: 'kept-second', title: 'Second' })
  })

  test("the comments kept in memory follow every change, a moderator's list has the hidden ones", async () => {
    // Before what is read: an account's change drops it all too, which would hide a missing drop
    const writer = await user('keptwriter')
    const answerer = await user('keptanswer')
    const created = await admin.post('/api/admin/posts', {
      slug: 'kept-comments', status: 'published', originalLang: 'en', translations: { en: { title: 'Talk', body: 'Text' } },
    })
    const target = `post:${created.json.id}`
    const first = (await writer.post('/api/comments', { target, body: 'First' })).json
    const reader = new Client()
    const read = async () => (await reader.get(`/api/comments?target=${target}`)).json.threads
    await read()
    assert.equal((await read())[0].body, 'First')

    const reply = await answerer.post('/api/comments', { target, body: 'A reply', replyTo: first.id })
    assert.equal(reply.status, 201)
    assert.deepEqual((await read())[0].replies.map((r) => r.body), ['A reply'])

    assert.equal((await writer.patch(`/api/comments/${first.id}`, { body: 'First, edited' })).status, 200)
    assert.equal((await read())[0].body, 'First, edited')

    const me = (await writer.get('/api/auth/me')).json.user
    assert.equal((await writer.patch('/api/auth/me', { displayName: 'Writer Renamed', locale: me.locale })).status, 200)
    assert.equal((await read())[0].author.displayName, 'Writer Renamed')

    assert.equal((await admin.patch(`/api/admin/comments/${reply.json.id}`, { status: 'hidden' })).status, 200)
    assert.deepEqual((await read())[0].replies, [])
    // The admin still sees it, the readers' list kept in memory isn't the admin's
    const moderated = (await admin.get(`/api/comments?target=${target}`)).json.threads
    assert.deepEqual(moderated[0].replies.map((r) => r.body), ['A reply'])
    assert.deepEqual((await read())[0].replies, [])
  })
})

describe('reactions', () => {
  test('toggle, counts for many targets, mine', async () => {
    const reader = await user('reactor')
    const other = await user('reactor2')
    const noteId = (await admin.post('/api/admin/notes', note())).json.id
    const comment = (await reader.post('/api/comments', { target: `note:${noteId}`, body: 'nice' })).json

    assert.equal((await new Client().post('/api/reactions', { target: `note:${noteId}`, kind: 'like', on: true })).status, 401)
    const invalid = await reader.post('/api/reactions', { target: 'guestbook', kind: 'meh', on: 'yes' })
    assert.deepEqual(Object.keys(invalid.json.error.fields).sort(), ['kind', 'on', 'target'])

    assert.deepEqual((await reader.post('/api/reactions', { target: `note:${noteId}`, kind: 'heart', on: true })).json, { counts: { heart: 1 }, mine: ['heart'] })
    // Twice is still once
    assert.deepEqual((await reader.post('/api/reactions', { target: `note:${noteId}`, kind: 'heart', on: true })).json.counts, { heart: 1 })
    await other.post('/api/reactions', { target: `note:${noteId}`, kind: 'heart', on: true })
    await other.post('/api/reactions', { target: `comment:${comment.id}`, kind: 'cat', on: true })
    await other.post('/api/reactions', { target: `comment:${comment.id}`, kind: 'thanks', on: true })
    // One's own comment too, like on GitHub
    await reader.post('/api/reactions', { target: `comment:${comment.id}`, kind: 'eyes', on: true })

    const targets = `note:${noteId},comment:${comment.id},post:999999`
    const seen = (await reader.get(`/api/reactions?targets=${targets}`)).json.reactions
    assert.deepEqual(seen[`note:${noteId}`], { counts: { heart: 2 }, mine: ['heart'] })
    assert.deepEqual(seen[`comment:${comment.id}`], { counts: { cat: 1, eyes: 1, thanks: 1 }, mine: ['eyes'] })
    assert.deepEqual(seen['post:999999'], { counts: {}, mine: [] })
    assert.deepEqual((await new Client().get(`/api/reactions?targets=note:${noteId}`)).json.reactions[`note:${noteId}`].mine, [])

    assert.deepEqual((await reader.post('/api/reactions', { target: `note:${noteId}`, kind: 'heart', on: false })).json, { counts: { heart: 1 }, mine: [] })
    assert.equal((await reader.post('/api/reactions', { target: 'note:999999', kind: 'like', on: true })).status, 404)
    assert.equal((await new Client().get('/api/reactions?targets=bad')).status, 400)

    // Deleting the note takes its reactions (and its comments' ones) along
    await admin.delete(`/api/admin/notes/${noteId}`)
    assert.deepEqual((await reader.get(`/api/reactions?targets=${targets}`)).json.reactions[`comment:${comment.id}`].counts, {})
  })
})
