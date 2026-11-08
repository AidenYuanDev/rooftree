// The admin's pages as console commands (ConsoleAdmin.h): posts, tags, notes, thinking, comments,
// chat, friends' links, projects, uploads, accounts and the numbers, checked against the API (pnpm test:api).

import assert from 'node:assert/strict'
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { after, before, describe, test } from 'node:test'
import { adminPassword, baseUrl, Client, lastMailTo, startServer, totp } from './server.mjs'

let server
let admin
let files

before(async () => {
  server = await startServer()
  admin = new Client()
  assert.equal((await admin.post('/api/auth/login', { login: 'author', password: adminPassword, totp: totp(server.adminTotpSecret) })).status, 200)
  files = mkdtempSync(join(tmpdir(), 'blog-console-admin-'))
})

after(async () => {
  if (process.exitCode && server)
    console.log(server.log.join(''))
  await server?.stop()
  rmSync(files, { recursive: true, force: true })
})

function file(name, text) {
  const path = join(files, name)
  writeFileSync(path, text)
  return path
}

async function reader(name) {
  const client = new Client()
  const email = `${name}@example.com`
  assert.equal((await client.post('/api/auth/register', { username: name, displayName: name, email, password: `password ${name}` })).status, 201)
  assert.equal((await client.post('/api/auth/verify-email', { token: lastMailTo(server.outbox, email).token })).status, 200)
  assert.equal((await client.post('/api/auth/login', { login: name, password: `password ${name}` })).status, 200)
  return client
}

describe('posts and tags', () => {
  test('tags', () => {
    assert.match(server.console('tag add console 控制台 Console'), /Added tag console/)
    assert.match(server.console('tag add console x y'), /slug: taken/)
    assert.match(server.console('tag edit console "控制 台" Console'), /Saved tag console/)
    assert.match(server.console('tag list'), /console +控制 台 +Console +0 posts/)
  })

  test('create, edit, translate, publish, list, delete', async () => {
    const created = server.console(`post create from-console zh "控制台 发的" ${file('a.md', '# 你好\n\n正文')}`)
    assert.match(created, /Created post from-console \(\d+\): draft\n {2}zh \(original\): 控制台 发的, 8 characters/)
    assert.match(server.console(`post create from-console zh x ${file('b.md', 'y')}`), /Nothing was saved:\n {2}slug: taken/)
    assert.match(server.console('post create bad-lang fr x /nowhere'), /Syntax: post create/)
    assert.match(server.console('post create no-file zh x /nowhere.md'), /\/nowhere\.md: No such file/)

    assert.match(server.console('post edit from-console tags console'), /tags: console/)
    assert.match(server.console('post edit from-console tags nothing-like-it'), /tags: unknown_tag/)
    assert.match(server.console('post edit from-console comments off'), /comments off/)
    assert.match(server.console('post edit from-console publish-at 2020-01-01 09:00+08:00'), /publishAt: past/)
    assert.match(server.console('post edit from-console publish-at tomorrow'), /A time with its zone/)
    assert.match(server.console(`post translation set from-console en "From the console" ${file('c.md', 'Hello')} A summary`),
      /en: From the console - A summary, 5 characters/)

    assert.match(server.console('post publish from-console'), /Published post from-console \(\d+\): published, published 20/)
    const post = (await new Client().get('/api/posts/from-console?lang=en')).json
    assert.equal(post.title, 'From the console')
    assert.deepEqual(post.tags.map((tag) => tag.slug ?? tag), ['console'])

    assert.match(server.console('post list published'), /from-console +en,zh|from-console +zh,en/)
    assert.match(server.console('post translation delete from-console zh'), /translations\.zh: required/)
    assert.match(server.console('post translation delete from-console en'), /Saved post/)
    assert.match(server.console('post show from-console'), /zh \(original\): 控制台 发的/)
    assert.match(server.console('post unpublish from-console'), /: draft/)
    assert.match(server.console('post delete from-console'), /Deleted from-console/)
    assert.match(server.console('post show from-console'), /There is no post from-console/)
    assert.match(server.console('tag delete console'), /Deleted tag console/)
  })
})

describe('notes and thinking', () => {
  test('notes', () => {
    const created = server.console(`note create zh 今天 ${file('n.md', '看星星')} published`)
    const id = created.match(/Created note (\d+): published/)[1]
    assert.match(server.console(`note edit ${id} mood 开心`), /mood: 开心, weather: -/)
    assert.match(server.console(`note translation set ${id} en Today ${file('n-en.md', 'Stars')}`), /en: Today, 5 characters/)
    assert.match(server.console('note list'), new RegExp(`${id} +published +(zh,en|en,zh) +今天`))
    assert.match(server.console(`note unpublish ${id}`), /: draft/)
    assert.match(server.console(`note delete ${id}`), new RegExp(`Deleted note ${id}`))
    assert.match(server.console(`note show ${id}`), /Not found/)
  })

  test('thinking', () => {
    const id = server.console('thinking add zh 人间有味是清欢').match(/Added thinking (\d+): published/)[1]
    assert.match(server.console(`thinking edit ${id} en The taste of life is in the plain`), /en: The taste of life/)
    assert.match(server.console('thinking list'), /人间有味是清欢/)
    assert.match(server.console(`thinking unpublish ${id}`), /: draft/)
    assert.match(server.console(`thinking delete ${id}`), new RegExp(`Deleted thinking ${id}`))
  })
})

describe('readers', () => {
  test('comments are hidden and shown again', async () => {
    const post = await admin.post('/api/admin/posts', { slug: 'commented', status: 'published', originalLang: 'zh', translations: { zh: { title: 't', body: 'b' } } })
    const alice = await reader('alice')
    const comment = (await alice.post('/api/comments', { postId: post.json.id, body: '控制台看得到我吗' })).json
    assert.match(server.console('comment list'), new RegExp(`${comment.id} +visible .* alice +commented +控制台看得到我吗`))
    assert.match(server.console(`comment hide ${comment.id}`), /is hidden now/)
    assert.match(server.console('comment list hidden'), /控制台看得到我吗/)
    assert.match(server.console(`comment show ${comment.id}`), /is visible now/)
    assert.match(server.console('comment hide 999999'), /Not found/)
  })

  test('comments whose passage the post lost, kept; a post\'s versions', async () => {
    const post = (await admin.post('/api/admin/posts', {
      slug: 'console-passages', status: 'published', originalLang: 'zh', translations: { zh: { title: '版本', body: '第一句话。' } },
    })).json
    const bob = await reader('pbob')
    const comment = (await bob.post('/api/comments', {
      target: `post:${post.id}`, body: '在问第一句', anchor: { lang: 'zh', block: 0, quote: '第一句话', prefix: '', suffix: '。', text: '第一句话。' },
    })).json
    await admin.patch(`/api/admin/posts/${post.id}`, { translations: { zh: { title: '版本', body: '换掉了。' } } })

    assert.match(server.console('comment list orphaned'), new RegExp(`${comment.id} +visible .* pbob .*在问第一句\n +on "第一句话" \\(zh, orphaned\\)`))
    assert.match(server.console(`comment keep ${comment.id}`), /keeps its passage/)
    assert.doesNotMatch(server.console('comment list orphaned'), /在问第一句/)
    assert.match(server.console('comment list'), /on "第一句话" \(zh, kept\)/)

    const versions = server.console('post revisions console-passages')
    assert.match(versions, /zh +published .* author +版本 +\(4 characters\)\n +\d+ +zh +published .* 版本 +\(5 characters\)/)
    const newest = versions.match(/^ *(\d+)/)[1]
    assert.match(server.console(`post revisions console-passages ${newest}`), /版本 +\(4 characters\)\n\n换掉了。/)
    assert.match(server.console('post revisions console-passages 999999'), /not found/i)
  })

  test('friends\' links', async () => {
    const neighbour = await reader('neighbour')
    const applied = (await neighbour.post('/api/friends', { name: '邻居', url: 'https://neighbour.example.com', description: '隔壁', message: '互链吗' })).json
    assert.match(server.console('friend list pending'), /邻居 .*https:\/\/neighbour\.example\.com +by neighbour .*\n +"互链吗"/)
    assert.match(server.console(`friend approve ${applied.id}`), /approved, the applicant is mailed/)
    assert.match(lastMailTo(server.outbox, 'neighbour@example.com').text, /邻居/)
    assert.match(server.console(`friend edit ${applied.id} description 隔壁的猫`), /Saved\n.*隔壁的猫/)

    const added = server.console('friend add 自己 https://me.example.com 我的另一个站')
    const id = added.match(/\n +(\d+) +approved/)[1]
    assert.equal((await new Client().get('/api/friends')).json.friends.length, 2)
    assert.match(server.console(`friend delete ${id}`), /Deleted link/)
    assert.match(server.console('friend add x ftp://nope'), /url: invalid/)
  })

  test('projects', () => {
    const id = server.console('project add Rooftree https://github.com/example/rooftree').match(/\n +(\d+) +Rooftree/)[1]
    assert.match(server.console(`project edit ${id} zh 自己的游戏服务器框架`), /Saved\n.*游戏服务器框架/)
    assert.match(server.console(`project edit ${id} repo https://github.com/example/rooftree`), /Saved/)
    assert.match(server.console('project list'), /Rooftree/)
    assert.match(server.console(`project delete ${id}`), /Deleted project/)
  })

  test('an upload', async () => {
    const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64')
    writeFileSync(join(files, 'dot.png'), png)
    const url = server.console(`upload ${join(files, 'dot.png')}`).match(/Uploaded (\/uploads\/\S+) \(image\/png/)[1]
    assert.deepEqual(Buffer.from(await (await fetch(baseUrl + url)).arrayBuffer()), png)
    assert.match(server.console(`upload ${file('not.png', 'not an image')}`), /file: unsupported_type/)
  })
})

describe('accounts and numbers', () => {
  test('create, show, role, deny, drive, kick, reset mail', async () => {
    assert.match(server.console('account create carol carol@example.com 卡罗尔'), /Created carol \(\d+\), a mail to carol@example.com/)
    assert.ok(lastMailTo(server.outbox, 'carol@example.com').token)
    assert.match(server.console('account show carol'), /carol \(\d+\), 卡罗尔 <carol@example.com> \(not confirmed\), user\n.*last seen never/)
    assert.match(server.console('account role carol moderator'), /carol is a moderator now/)
    assert.match(server.console('account deny carol comment'), /carol may not: comment/)
    assert.match(server.console('account deny carol avatar'), /carol may not: comment, avatar/)
    assert.match(server.console('account allow carol comment'), /carol may not: avatar/)
    assert.match(server.console('account drive carol on 5G 3'), /drive: 0 KB of 5\.0 GB, trash 3 days/)
    assert.match(server.console('account drive carol on default'), /drive: 0 KB of .* \(default\), trash 3 days/)
    assert.match(server.console('account drive carol off'), /drive: off/)

    const bob = await reader('bob')
    assert.equal((await bob.get('/api/auth/me')).json.user.username, 'bob')
    assert.match(server.console('account show bob'), /1 logged in devices/)
    assert.match(server.console('account kick bob'), /bob is logged out everywhere/)
    assert.equal((await bob.get('/api/auth/me')).json.user, null)
    assert.match(server.console('account reset-mail bob'), /mailed to bob/)
    assert.match(server.console('account show nobody'), /There is no account nobody/)
  })

  test('online, stats, status', () => {
    assert.match(server.console('online'), /\d+ pages open, \d+ accounts logged in/)
    assert.match(server.console('stats 3'), /\d+ views, \d+ visitors a day added up:\n.*\n.*\n .*\(today\) +\d+ views/)
    assert.match(server.console('status'), /Up \d+d \d+h \d+m, .* requests a minute\nUpdate .* CPU \d+\.\d% of \d+ cores, load \d+\.\d,/)
  })

  test('help lists them', () => {
    assert.match(server.console('help'), /post/)
    assert.match(server.console('help post'), /translation/)
    assert.match(server.console('help account'), /reset-mail/)
  })
})

describe('chat', () => {
  test('groups, messages, delete, clear, dissolve', async () => {
    const dora = await reader('dora')
    const said = (await dora.post('/api/chat/channels/square/messages', { content: '控制台看得到广场吗' })).json
    assert.match(server.console('chat messages square'), new RegExp(`${said.id} .* dora +控制台看得到广场吗`))
    assert.match(server.console('chat messages dm:1:2'), /Syntax: chat messages/)
    assert.match(server.console(`chat delete ${said.id}`), new RegExp(`Chat message ${said.id} deleted`))
    assert.match(server.console(`chat delete ${said.id}`), /Not found/)
    assert.match(server.console('chat messages square'), /No messages/)

    const group = (await dora.post('/api/chat/groups', { name: '控制台的群', memberIds: [] })).json
    const id = group.id.slice('group:'.length)
    await dora.post(`/api/chat/channels/${group.id}/messages`, { content: '群里一句' })
    await dora.post(`/api/chat/channels/${group.id}/messages`, { content: '群里两句' })
    assert.match(server.console('chat groups'), new RegExp(`${id} +控制台的群 +owner dora +1 members +2 messages`))
    assert.match(server.console(`chat messages ${id}`), /群里两句\n.*群里一句/)
    assert.match(server.console(`chat clear group:${id}`), new RegExp(`Cleared group:${id}: 2 messages removed`))
    assert.match(server.console(`chat dissolve ${id}`), new RegExp(`Dissolved group ${id}: 0 messages removed`))
    assert.match(server.console('chat groups'), /No groups/)
    assert.match(server.console('chat dissolve square'), /Syntax: chat dissolve/)
  })

  test('chat taken from an account', async () => {
    await reader('erin')
    assert.match(server.console('account deny erin chat'), /erin may not: chat/)
    assert.match(server.console('account allow erin chat'), /erin may not: -/)
  })
})
