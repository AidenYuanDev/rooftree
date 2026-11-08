// Accounts' drives: who has one, folders, uploads in pieces, downloads with Range, the quota, and
// links others open (pnpm test:api)

import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import { existsSync, mkdtempSync, readdirSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { after, before, describe, test } from 'node:test'
import { adminPassword, Client, startServer, totp } from './server.mjs'

let server
let admin
let driveDir

before(async () => {
  server = await startServer()
  driveDir = join(dirname(server.outbox), 'drive')
  admin = new Client()
  assert.equal((await admin.post('/api/auth/login', { login: 'author', password: adminPassword, totp: totp(server.adminTotpSecret) })).status, 200)
})

after(async () => {
  if (process.exitCode && server)
    console.log(server.log.join(''))
  await server?.stop()
})

// An account made by the admin, logged in; drive: given one of 100000 bytes
async function made(name, { drive = false } = {}) {
  const created = await admin.post('/api/admin/users', {
    username: name, displayName: `${name} 名`, email: `${name}@example.com`, password: `password ${name}`, emailVerified: true,
  })
  assert.equal(created.status, 201, JSON.stringify(created.json))
  if (drive)
    assert.equal((await admin.put(`/api/admin/users/${created.json.id}/drive`, { enabled: true, quota: 100000 })).status, 200)
  const client = new Client()
  assert.equal((await client.post('/api/auth/login', { login: name, password: `password ${name}` })).status, 200)
  return { id: created.json.id, client }
}

const bytes = (size, seed = 1) => Buffer.from(Array.from({ length: size }, (_, i) => (i * 7 + seed) % 256))

// Uploads data in pieces of pieceSize, the new file back
async function upload(client, name, data, { folderId = null, pieceSize = 4096 } = {}) {
  const created = await client.post('/api/drive/uploads', { folderId, name, size: data.length })
  assert.equal(created.status, 201, JSON.stringify(created.json))
  for (let offset = 0; offset < data.length; offset += pieceSize) {
    const piece = await client.binary('PUT', `/api/drive/uploads/${created.json.id}?offset=${offset}`, data.subarray(offset, offset + pieceSize))
    assert.equal(piece.status, 200, JSON.stringify(piece.json))
  }
  const done = await client.post(`/api/drive/uploads/${created.json.id}/complete`)
  assert.equal(done.status, 201, JSON.stringify(done.json))
  return done.json
}

// A zip read by Python's zipfile, which checks every CRC: {name: base64 of its bytes}
function unzip(data) {
  const dir = mkdtempSync(join(tmpdir(), 'blog-zip-'))
  try {
    writeFileSync(join(dir, 'a.zip'), data)
    const script = `import base64, json, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
assert z.testzip() is None
print(json.dumps({i.filename: base64.b64encode(z.read(i)).decode() for i in z.infolist()}))`
    return JSON.parse(execFileSync('python3', ['-I', '-c', script, join(dir, 'a.zip')]).toString())
  } finally {
    rmSync(dir, { recursive: true })
  }
}

const storedFiles = () => (existsSync(join(driveDir, 'files')) ? readdirSync(join(driveDir, 'files')) : [])

describe('who has a drive', () => {
  test('the admin, and accounts the admin gave one', async () => {
    assert.equal((await admin.get('/api/auth/me')).json.user.drive, true)
    assert.equal((await admin.get('/api/drive')).status, 200)

    const { id, client } = await made('nadia')
    assert.equal((await client.get('/api/auth/me')).json.user.drive, false)
    assert.equal((await client.get('/api/drive')).json.error.code, 'no_drive')
    assert.equal((await new Client().get('/api/drive')).status, 401)

    const given = await admin.put(`/api/admin/users/${id}/drive`, { enabled: true, quota: 5000 })
    assert.equal(given.status, 200)
    assert.deepEqual(given.json.drive, { enabled: true, quota: 5000, defaultQuota: 0, used: 0, trashDays: null, defaultTrashDays: 1 })
    assert.equal((await client.get('/api/auth/me')).json.user.drive, true)
    const listed = await client.get('/api/drive')
    assert.equal(listed.status, 200)
    assert.deepEqual(listed.json.usage, { used: 0, quota: 5000, maxFileSize: 60000, chunkSize: 8 * 1024 * 1024, trashDays: 1 })

    assert.equal((await admin.put(`/api/admin/users/${id}/drive`, { enabled: false, quota: null })).status, 200)
    assert.equal((await client.get('/api/drive')).status, 403)
    assert.equal((await client.put(`/api/admin/users/${id}/drive`, { enabled: true })).status, 403)
  })

  test("every drive starts empty, the admin's too, until the admin gives it a size", async () => {
    const { id, client } = await made('zero')
    await admin.put(`/api/admin/users/${id}/drive`, { enabled: true, quota: null })
    assert.equal((await client.get('/api/drive')).json.usage.quota, 0)
    assert.equal((await client.post('/api/drive/uploads', { folderId: null, name: 'a', size: 1 })).json.error.code, 'quota_exceeded')
    // An empty file fits
    assert.equal((await client.post('/api/drive/uploads', { folderId: null, name: 'empty', size: 0 })).status, 201)

    const adminId = (await admin.get('/api/auth/me')).json.user.id
    assert.equal((await admin.get('/api/drive')).json.usage.quota, 0)
    assert.equal((await admin.post('/api/drive/uploads', { folderId: null, name: 'a', size: 1 })).status, 507)
    const own = await admin.put(`/api/admin/users/${adminId}/drive`, { enabled: true, quota: 3000 })
    assert.equal(own.status, 200)
    assert.deepEqual(own.json.drive, { enabled: true, quota: 3000, defaultQuota: 0, used: 0, trashDays: null, defaultTrashDays: 1 })
    assert.equal((await admin.get('/api/drive')).json.usage.quota, 3000)
    assert.equal((await admin.post('/api/drive/uploads', { folderId: null, name: 'a', size: 1 })).status, 201)
  })
})

describe('folders and files', () => {
  test('made, renamed, moved, never into themselves; names unique per folder', async () => {
    const { client } = await made('fiona', { drive: true })
    const docs = (await client.post('/api/drive/folders', { parentId: null, name: 'docs' })).json
    const inner = (await client.post('/api/drive/folders', { parentId: docs.id, name: 'inner' })).json
    assert.equal(inner.parentId, docs.id)

    assert.equal((await client.post('/api/drive/folders', { parentId: null, name: 'docs' })).status, 409)
    // Names are compared byte for byte
    assert.equal((await client.post('/api/drive/folders', { parentId: null, name: 'Docs' })).status, 201)
    for (const name of ['', '.', '..', 'a/b', 'a\\b', 'tab\there', 'x'.repeat(256)])
      assert.equal((await client.post('/api/drive/folders', { parentId: null, name })).status, 400, name)

    assert.equal((await client.patch(`/api/drive/folders/${docs.id}`, { parentId: inner.id })).json.error.fields.parentId, 'invalid')
    assert.equal((await client.patch(`/api/drive/folders/${docs.id}`, { parentId: docs.id })).json.error.fields.parentId, 'invalid')
    const moved = await client.patch(`/api/drive/folders/${inner.id}`, { parentId: null, name: 'top' })
    assert.equal(moved.status, 200)
    assert.deepEqual([moved.json.parentId, moved.json.name], [null, 'top'])

    const file = await upload(client, 'note.txt', Buffer.from('hello'), { folderId: docs.id })
    assert.deepEqual([file.folderId, file.name, file.size, file.mime], [docs.id, 'note.txt', 5, 'text/plain'])
    // The same name again: numbered
    assert.equal((await upload(client, 'note.txt', Buffer.from('again'), { folderId: docs.id })).name, 'note (2).txt')

    const renamed = await client.patch(`/api/drive/files/${file.id}`, { name: 'photo.png', folderId: null })
    assert.deepEqual([renamed.json.name, renamed.json.mime, renamed.json.folderId], ['photo.png', 'image/png', null])
    assert.equal((await client.patch(`/api/drive/files/${file.id}`, { name: 'Docs' })).status, 409)

    const root = await client.get('/api/drive')
    assert.deepEqual(root.json.folders.map((f) => f.name), ['Docs', 'docs', 'top'])
    assert.deepEqual(root.json.files.map((f) => f.name), ['photo.png'])
    assert.equal(root.json.usage.used, 10)
    const inDocs = await client.get(`/api/drive?folder=${docs.id}`)
    assert.deepEqual(inDocs.json.path, [{ id: docs.id, name: 'docs' }])
    assert.deepEqual(inDocs.json.files.map((f) => f.name), ['note (2).txt'])
    assert.equal((await client.get('/api/drive/folders')).json.folders.length, 3)
  })

  test("nobody reaches another account's", async () => {
    const { client: owner } = await made('olga', { drive: true })
    const { client: other } = await made('oscar', { drive: true })
    const folder = (await owner.post('/api/drive/folders', { parentId: null, name: 'mine' })).json
    const file = await upload(owner, 'a.bin', bytes(10))

    assert.equal((await other.get(`/api/drive?folder=${folder.id}`)).status, 404)
    assert.equal((await other.binary('GET', `/api/drive/files/${file.id}`)).status, 404)
    assert.equal((await other.patch(`/api/drive/files/${file.id}`, { name: 'x' })).status, 404)
    assert.equal((await other.delete(`/api/drive/files/${file.id}`)).status, 404)
    assert.equal((await other.delete(`/api/drive/folders/${folder.id}`)).status, 404)
    assert.equal((await other.post('/api/drive/folders', { parentId: folder.id, name: 'in' })).status, 404)
    assert.equal((await other.post('/api/drive/uploads', { folderId: folder.id, name: 'x', size: 1 })).status, 404)
    assert.equal((await other.patch(`/api/drive/files/${(await upload(other, 'b.bin', bytes(3))).id}`, { folderId: folder.id })).status, 404)
    // The admin neither
    assert.equal((await admin.binary('GET', `/api/drive/files/${file.id}`)).status, 404)
  })

  test('a folder deleted takes what is under it to the trash, then off the disk', async () => {
    const { client } = await made('dora', { drive: true })
    const top = (await client.post('/api/drive/folders', { parentId: null, name: 'top' })).json
    const sub = (await client.post('/api/drive/folders', { parentId: top.id, name: 'sub' })).json
    const before = storedFiles().length
    const one = await upload(client, 'one', bytes(100), { folderId: top.id })
    await upload(client, 'two', bytes(100), { folderId: sub.id })
    const kept = await upload(client, 'kept', bytes(100))
    assert.equal(storedFiles().length, before + 3)

    const deleted = await client.delete(`/api/drive/folders/${top.id}`)
    assert.deepEqual(deleted.json, { ok: true, trashed: true })
    // Gone from the drive, everything under it too, still on the disk and counted
    assert.equal((await client.get(`/api/drive?folder=${sub.id}`)).status, 404)
    assert.equal((await client.binary('GET', `/api/drive/files/${one.id}`)).status, 404)
    assert.equal((await client.patch(`/api/drive/files/${one.id}`, { name: 'x' })).status, 404)
    assert.equal((await client.post('/api/drive/uploads', { folderId: sub.id, name: 'late', size: 1 })).status, 404)
    assert.deepEqual((await client.get('/api/drive')).json.folders, [])
    assert.equal((await client.get('/api/drive')).json.usage.used, 300)
    assert.equal(storedFiles().length, before + 3)

    const trash = (await client.get('/api/drive/trash')).json
    assert.equal(trash.days, 1)
    assert.deepEqual(trash.trash.map((item) => [item.kind, item.name, item.size, item.from]), [['folder', 'top', 200, []]])
    assert.equal(Date.parse(trash.trash[0].purgeAt) - Date.parse(trash.trash[0].trashedAt), 86400000)

    assert.equal((await client.delete(`/api/drive/trash/folder/${top.id}`)).status, 200)
    assert.equal((await client.delete(`/api/drive/trash/folder/${top.id}`)).status, 404)
    assert.equal(storedFiles().length, before + 1)
    assert.equal((await client.get('/api/drive')).json.usage.used, 100)

    // Not in the trash: not purged from there
    assert.equal((await client.delete(`/api/drive/trash/file/${kept.id}`)).status, 404)
    assert.equal((await client.delete(`/api/drive/files/${kept.id}`)).status, 200)
    assert.equal((await client.delete('/api/drive/trash')).status, 200)
    assert.deepEqual((await client.get('/api/drive/trash')).json.trash, [])
    assert.equal(storedFiles().length, before)
  })
})

describe('zips', () => {
  test('a folder, a choice or the whole drive, made as it goes out', async () => {
    const { client } = await made('zelda', { drive: true })
    const { client: other } = await made('zack', { drive: true })
    const photos = (await client.post('/api/drive/folders', { parentId: null, name: 'photos' })).json
    const year = (await client.post('/api/drive/folders', { parentId: photos.id, name: '2024 年' })).json
    await client.post('/api/drive/folders', { parentId: null, name: 'empty' })
    const a = await upload(client, 'a.jpg', bytes(5000, 2), { folderId: photos.id })
    const b = await upload(client, 'b.txt', Buffer.from('bee'), { folderId: year.id })
    const c = await upload(client, 'c.bin', bytes(0))
    const d = await upload(client, 'a.jpg', bytes(7, 9))

    const zip = await client.binary('GET', `/api/drive/zip?folder=${photos.id}`)
    assert.equal(zip.status, 200)
    assert.equal(zip.headers.get('content-type'), 'application/zip')
    assert.match(zip.headers.get('content-disposition'), /^attachment; filename="photos.zip"/)
    assert.equal(Number(zip.headers.get('content-length')), zip.data.length)
    const files = unzip(zip.data)
    assert.deepEqual(Object.keys(files), ['photos/', 'photos/2024 年/', 'photos/2024 年/b.txt', 'photos/a.jpg'])
    assert.equal(Buffer.from(files['photos/a.jpg'], 'base64').compare(bytes(5000, 2)), 0)
    assert.equal(Buffer.from(files['photos/2024 年/b.txt'], 'base64').toString(), 'bee')

    const whole = unzip((await client.binary('GET', '/api/drive/zip')).data)
    assert.deepEqual(Object.keys(whole).sort(), ['a.jpg', 'c.bin', 'empty/', 'photos/', 'photos/2024 年/', 'photos/2024 年/b.txt', 'photos/a.jpg'])

    // Chosen from two folders: the second a.jpg is a (2).jpg
    const chosen = await client.binary('GET', `/api/drive/zip?files=${a.id},${d.id},${c.id}&folders=${year.id}&name=picked`)
    assert.match(chosen.headers.get('content-disposition'), /filename="picked.zip"/)
    assert.deepEqual(Object.keys(unzip(chosen.data)), ['2024 年/', '2024 年/b.txt', 'a.jpg', 'a (2).jpg', 'c.bin'])

    // Not its own, or in the trash
    assert.equal((await other.binary('GET', `/api/drive/zip?files=${a.id}`)).status, 404)
    assert.equal((await other.binary('GET', `/api/drive/zip?folder=${photos.id}`)).status, 404)
    assert.equal((await client.binary('GET', '/api/drive/zip?files=x')).status, 404)
    await client.delete(`/api/drive/files/${b.id}`)
    assert.equal((await client.binary('GET', `/api/drive/zip?files=${b.id}`)).status, 404)
    assert.deepEqual(Object.keys(unzip((await client.binary('GET', `/api/drive/zip?folder=${year.id}`)).data)), ['2024 年/'])
  })

  test("a folder link's, counted as a download", async () => {
    const { client: owner } = await made('yara', { drive: true })
    const { client: viewer } = await made('yves')
    const top = (await owner.post('/api/drive/folders', { parentId: null, name: 'top' })).json
    const inner = (await owner.post('/api/drive/folders', { parentId: top.id, name: 'inner' })).json
    const outside = (await owner.post('/api/drive/folders', { parentId: null, name: 'outside' })).json
    const file = await upload(owner, 'x.txt', Buffer.from('x'), { folderId: inner.id })
    const link = (await owner.post('/api/drive/shares', { folderId: top.id, expires: '7d' })).json
    const fileLink = (await owner.post('/api/drive/shares', { fileId: file.id, expires: '7d' })).json

    assert.deepEqual(Object.keys(unzip((await viewer.binary('GET', `/api/shares/${link.token}/zip`)).data)), ['top/', 'top/inner/', 'top/inner/x.txt'])
    assert.deepEqual(Object.keys(unzip((await viewer.binary('GET', `/api/shares/${link.token}/zip?folder=${inner.id}`)).data)), ['inner/', 'inner/x.txt'])
    assert.equal((await viewer.binary('GET', `/api/shares/${link.token}/zip?folder=${outside.id}`)).status, 404)
    assert.equal((await viewer.binary('GET', `/api/shares/${fileLink.token}/zip`)).status, 404)
    const listed = (await owner.get('/api/drive/shares')).json.shares.find((share) => share.id === link.id)
    assert.equal(listed.downloads, 2)
  })
})

describe('small pictures', () => {
  test("made by the browser, kept by the server, shown through the drive and its links", async () => {
    const { client: owner } = await made('pippa', { drive: true })
    const { client: viewer } = await made('percy')
    const { client: other } = await made('pamela', { drive: true })
    const photo = await upload(owner, 'cat.jpg', bytes(300))
    const text = await upload(owner, 'notes.txt', bytes(3))
    assert.equal(photo.thumb, false)
    const webp = Buffer.concat([Buffer.from('RIFF\0\0\0\0WEBPVP8 '), bytes(50)])

    assert.equal((await owner.binary('PUT', `/api/drive/files/${photo.id}/thumb`, Buffer.from('<svg onload=alert(1)>'))).status, 400)
    assert.equal((await owner.binary('PUT', `/api/drive/files/${photo.id}/thumb`, Buffer.alloc(200 * 1024, 0xff))).status, 400)
    assert.equal((await owner.binary('PUT', `/api/drive/files/${text.id}/thumb`, webp)).status, 400)
    assert.equal((await other.binary('PUT', `/api/drive/files/${photo.id}/thumb`, webp)).status, 404)
    assert.equal((await owner.binary('GET', `/api/drive/files/${photo.id}/thumb`)).status, 404)
    assert.equal((await owner.binary('PUT', `/api/drive/files/${photo.id}/thumb`, webp)).status, 200)

    assert.equal((await owner.get('/api/drive')).json.files.find((file) => file.id === photo.id).thumb, true)
    const thumb = await owner.binary('GET', `/api/drive/files/${photo.id}/thumb`)
    assert.equal(thumb.status, 200)
    assert.equal(thumb.headers.get('content-type'), 'image/webp')
    assert.equal(thumb.data.compare(webp), 0)
    assert.equal((await other.binary('GET', `/api/drive/files/${photo.id}/thumb`)).status, 404)

    const link = (await owner.post('/api/drive/shares', { fileId: photo.id, expires: '7d' })).json.token
    assert.equal((await viewer.binary('GET', `/api/shares/${link}/files/${photo.id}/thumb`)).data.compare(webp), 0)
    assert.equal((await viewer.binary('GET', `/api/shares/${link}/files/${text.id}/thumb`)).status, 404)

    // Goes with its file
    const before = existsSync(join(driveDir, 'thumbs')) ? readdirSync(join(driveDir, 'thumbs')).length : 0
    await owner.delete(`/api/drive/files/${photo.id}`)
    await owner.delete(`/api/drive/trash/file/${photo.id}`)
    assert.equal(readdirSync(join(driveDir, 'thumbs')).length, before - 1)
  })
})

describe('the trash', () => {
  test('restores where it was, or to the root, by a name not taken', async () => {
    const { client } = await made('rhea', { drive: true })
    const docs = (await client.post('/api/drive/folders', { parentId: null, name: 'docs' })).json
    const old = (await client.post('/api/drive/folders', { parentId: docs.id, name: 'old' })).json
    const a = await upload(client, 'a.txt', bytes(10), { folderId: docs.id })
    const b = await upload(client, 'b.txt', bytes(20), { folderId: old.id })

    await client.delete(`/api/drive/files/${a.id}`)
    // Its name is free again meanwhile
    const again = await upload(client, 'a.txt', bytes(5), { folderId: docs.id })
    assert.equal(again.name, 'a.txt')
    const restored = await client.post(`/api/drive/trash/file/${a.id}/restore`)
    assert.equal(restored.status, 200, JSON.stringify(restored.json))
    assert.deepEqual([restored.json.kind, restored.json.name, restored.json.folderId], ['file', 'a (2).txt', docs.id])
    assert.equal((await client.post(`/api/drive/trash/file/${a.id}/restore`)).status, 404)

    // b first, then its folder: b's folder is in the trash, b comes back to the root
    await client.delete(`/api/drive/files/${b.id}`)
    await client.delete(`/api/drive/folders/${old.id}`)
    const trash = (await client.get('/api/drive/trash')).json.trash
    assert.deepEqual(trash.map((item) => [item.kind, item.name, item.size, item.from]), [['folder', 'old', 20, ['docs']], ['file', 'b.txt', 20, ['docs', 'old']]])
    assert.equal((await client.post(`/api/drive/trash/file/${b.id}/restore`)).json.folderId, null)
    assert.equal((await client.post(`/api/drive/trash/folder/${old.id}/restore`)).json.parentId, docs.id)
    assert.deepEqual((await client.get(`/api/drive?folder=${old.id}`)).json.files, [])
  })

  test("hides links to what it holds until restored; the admin sets an account's days", async () => {
    const { id, client: owner } = await made('tess', { drive: true })
    const { client: viewer } = await made('vic')
    const folder = (await owner.post('/api/drive/folders', { parentId: null, name: 'shared' })).json
    const file = await upload(owner, 'in.txt', bytes(5), { folderId: folder.id })
    const link = (await owner.post('/api/drive/shares', { fileId: file.id, expires: '7d' })).json.token
    assert.equal((await viewer.get(`/api/shares/${link}`)).status, 200)

    await owner.delete(`/api/drive/folders/${folder.id}`)
    assert.equal((await viewer.get(`/api/shares/${link}`)).status, 404)
    assert.equal((await viewer.binary('GET', `/api/shares/${link}/files/${file.id}`)).status, 404)
    assert.deepEqual((await owner.get('/api/drive/shares')).json.shares.map((share) => share.trashed), [true])
    await owner.post(`/api/drive/trash/folder/${folder.id}/restore`)
    assert.equal((await viewer.get(`/api/shares/${link}`)).status, 200)
    assert.deepEqual((await owner.get('/api/drive/shares')).json.shares.map((share) => share.trashed), [false])

    // None: deleted at once
    const set = await admin.put(`/api/admin/users/${id}/drive`, { enabled: true, quota: 100000, trashDays: 0 })
    assert.equal(set.json.drive.trashDays, 0)
    assert.equal((await admin.put(`/api/admin/users/${id}/drive`, { enabled: true, trashDays: 366 })).status, 400)
    assert.equal((await owner.get('/api/drive')).json.usage.trashDays, 0)
    assert.deepEqual((await owner.delete(`/api/drive/files/${file.id}`)).json, { ok: true, trashed: false })
    assert.deepEqual((await owner.get('/api/drive/trash')).json, { trash: [], days: 0 })
    assert.equal((await owner.get('/api/drive/shares')).json.shares.length, 0)
  })
})

describe('uploads and downloads', () => {
  test('in pieces, from where the server got to, then whole or a range', async () => {
    const { client } = await made('ursula', { drive: true })
    const data = bytes(10000, 3)
    const created = (await client.post('/api/drive/uploads', { folderId: null, name: 'clip.mp4', size: data.length })).json
    assert.equal((await client.binary('PUT', `/api/drive/uploads/${created.id}?offset=0`, data.subarray(0, 4000))).json.received, 4000)
    // A piece sent again (its answer was lost): the server says where it is
    const again = await client.binary('PUT', `/api/drive/uploads/${created.id}?offset=0`, data.subarray(0, 4000))
    assert.equal(again.status, 409)
    assert.equal(again.json.error.fields.received, '4000')
    assert.equal((await client.post(`/api/drive/uploads/${created.id}/complete`)).json.error.code, 'incomplete')
    // Past its size
    assert.equal((await client.binary('PUT', `/api/drive/uploads/${created.id}?offset=4000`, bytes(7000))).status, 400)
    // Unfinished, it counts
    assert.equal((await client.get('/api/drive')).json.usage.used, 10000)
    assert.equal((await client.binary('PUT', `/api/drive/uploads/${created.id}?offset=4000`, data.subarray(4000))).json.received, 10000)
    const file = (await client.post(`/api/drive/uploads/${created.id}/complete`)).json
    assert.equal(file.size, 10000)
    assert.equal((await client.post(`/api/drive/uploads/${created.id}/complete`)).status, 404)

    const whole = await client.binary('GET', `/api/drive/files/${file.id}`)
    assert.equal(whole.status, 200)
    assert.ok(whole.data.equals(data))
    assert.equal(whole.headers.get('content-type'), 'video/mp4')
    assert.equal(whole.headers.get('accept-ranges'), 'bytes')
    assert.match(whole.headers.get('content-disposition'), /^inline; filename="clip.mp4"/)
    assert.equal(whole.headers.get('content-security-policy'), "sandbox; frame-ancestors 'self'")

    const part = await client.binary('GET', `/api/drive/files/${file.id}`, undefined, { Range: 'bytes=100-199' })
    assert.equal(part.status, 206)
    assert.equal(part.headers.get('content-range'), 'bytes 100-199/10000')
    assert.ok(part.data.equals(data.subarray(100, 200)))
    const tail = await client.binary('GET', `/api/drive/files/${file.id}`, undefined, { Range: 'bytes=-10' })
    assert.ok(tail.data.equals(data.subarray(9990)))
    const past = await client.binary('GET', `/api/drive/files/${file.id}`, undefined, { Range: 'bytes=10000-' })
    assert.equal(past.status, 416)
    assert.equal(past.headers.get('content-range'), 'bytes */10000')

    const download = await client.binary('GET', `/api/drive/files/${file.id}?download=1`)
    assert.equal(download.headers.get('content-type'), 'application/octet-stream')
    assert.match(download.headers.get('content-disposition'), /^attachment;/)
  })

  test('a page or a picture that runs scripts is never shown as the site', async () => {
    const { client } = await made('hal', { drive: true })
    for (const name of ['page.html', 'logo.svg', '页面.htm']) {
      const file = await upload(client, name, Buffer.from('<script>alert(1)</script>'))
      const response = await client.binary('GET', `/api/drive/files/${file.id}`)
      assert.equal(response.headers.get('content-type'), 'application/octet-stream', name)
      assert.match(response.headers.get('content-disposition'), /^attachment;/, name)
      assert.equal(response.headers.get('x-content-type-options'), 'nosniff')
    }
    const file = (await client.get('/api/drive')).json.files.find((f) => f.name === '页面.htm')
    const response = await client.binary('GET', `/api/drive/files/${file.id}`)
    assert.match(response.headers.get('content-disposition'), /filename\*=UTF-8''%E9%A1%B5%E9%9D%A2\.htm$/)
  })

  test('the quota and the largest file; cancelled uploads give their room back', async () => {
    const { id, client } = await made('quinn', { drive: true })
    assert.equal((await client.post('/api/drive/uploads', { folderId: null, name: 'big', size: 60001 })).json.error.fields.size, 'too_long')
    await upload(client, 'a', bytes(60000), { pieceSize: 30000 })
    const full = await client.post('/api/drive/uploads', { folderId: null, name: 'b', size: 50000 })
    assert.equal(full.status, 507)
    assert.equal(full.json.error.code, 'quota_exceeded')

    const pending = (await client.post('/api/drive/uploads', { folderId: null, name: 'c', size: 40000 })).json
    assert.equal((await client.post('/api/drive/uploads', { folderId: null, name: 'd', size: 1 })).status, 507)
    assert.equal((await client.delete(`/api/drive/uploads/${pending.id}`)).status, 200)
    assert.equal((await client.post('/api/drive/uploads', { folderId: null, name: 'd', size: 1 })).status, 201)

    // The admin gives it more
    await admin.put(`/api/admin/users/${id}/drive`, { enabled: true, quota: 150000 })
    assert.equal((await client.post('/api/drive/uploads', { folderId: null, name: 'b', size: 50000 })).status, 201)
    assert.equal((await admin.get(`/api/admin/users/${id}`)).json.drive.used, 60000)
  })
})

describe('links', () => {
  test('a file with a password: locked until given, counted, revoked', async () => {
    const { client: owner } = await made('sam', { drive: true })
    const { id: viewerId, client: viewer } = await made('vera')
    const data = bytes(3000)
    const file = await upload(owner, 'report.pdf', data)

    const share = await owner.post('/api/drive/shares', { fileId: file.id, expires: '7d', password: 'open sesame' })
    assert.equal(share.status, 201)
    assert.equal(share.json.kind, 'file')
    assert.equal(share.json.name, 'report.pdf')
    assert.equal(share.json.hasPassword, true)
    assert.equal(share.json.token.length, 32)
    const { token } = share.json

    assert.equal((await new Client().get(`/api/shares/${token}`)).status, 401)
    const locked = await viewer.get(`/api/shares/${token}`)
    assert.equal(locked.status, 200)
    assert.deepEqual([locked.json.share.locked, locked.json.share.owner, locked.json.file], [true, 'sam 名', undefined])
    assert.equal((await viewer.binary('GET', `/api/shares/${token}/files/${file.id}`)).json.error.code, 'share_locked')

    assert.equal((await viewer.post(`/api/shares/${token}/unlock`, { password: 'wrong' })).json.error.code, 'wrong_password')
    assert.equal((await viewer.post(`/api/shares/${token}/unlock`, { password: 'open sesame' })).status, 200)
    const open = await viewer.get(`/api/shares/${token}`)
    assert.equal(open.json.share.locked, false)
    assert.equal(open.json.file.name, 'report.pdf')

    const shown = await viewer.binary('GET', `/api/shares/${token}/files/${file.id}`)
    assert.ok(shown.data.equals(data))
    // The browser's PDF viewer: not sandboxed
    assert.equal(shown.headers.get('content-type'), 'application/pdf')
    assert.doesNotMatch(shown.headers.get('content-security-policy'), /sandbox|object-src 'none'/)
    assert.equal((await viewer.binary('GET', `/api/shares/${token}/files/${file.id}?download=1`)).status, 200)
    // Only that file
    const other = await upload(owner, 'other.txt', Buffer.from('secret'))
    assert.equal((await viewer.binary('GET', `/api/shares/${token}/files/${other.id}`)).status, 404)

    // The owner neither needs the password nor counts
    assert.equal((await owner.get(`/api/shares/${token}`)).json.share.isOwner, true)
    const listed = (await owner.get('/api/drive/shares')).json.shares.find((s) => s.token === token)
    // Not while it was locked
    assert.deepEqual([listed.visitors, listed.views, listed.downloads], [1, 1, 1])
    const access = (await owner.get(`/api/drive/shares/${listed.id}/access`)).json.access
    assert.deepEqual(access.map((a) => [a.userId, a.username, a.views, a.downloads]), [[viewerId, 'vera', 1, 1]])
    // Not someone else's links
    assert.equal((await admin.get(`/api/drive/shares/${listed.id}/access`)).json.access.length, 0)
    assert.equal((await admin.delete(`/api/drive/shares/${listed.id}`)).status, 404)

    assert.equal((await owner.delete(`/api/drive/shares/${listed.id}`)).status, 200)
    assert.equal((await viewer.get(`/api/shares/${token}`)).status, 404)
  })

  test('wrong passwords are limited per account and link', async () => {
    const { client: owner } = await made('pia', { drive: true })
    const { client: guesser } = await made('gus')
    const file = await upload(owner, 'x', bytes(1))
    const { token } = (await owner.post('/api/drive/shares', { fileId: file.id, expires: 'never', password: 'right' })).json
    for (let i = 0; i < 5; ++i)
      assert.equal((await guesser.post(`/api/shares/${token}/unlock`, { password: `guess ${i}` })).status, 403)
    const limited = await guesser.post(`/api/shares/${token}/unlock`, { password: 'right' })
    assert.equal(limited.status, 429)
    assert.ok(Number(limited.headers.get('retry-after')) > 0)
  })

  test('a folder: what is under it, nothing above it', async () => {
    const { client: owner } = await made('felix', { drive: true })
    const { client: viewer } = await made('wendy')
    const top = (await owner.post('/api/drive/folders', { parentId: null, name: 'top' })).json
    const shared = (await owner.post('/api/drive/folders', { parentId: top.id, name: 'shared' })).json
    const sub = (await owner.post('/api/drive/folders', { parentId: shared.id, name: 'sub' })).json
    const inside = await upload(owner, 'inside.txt', Buffer.from('in'), { folderId: sub.id })
    const above = await upload(owner, 'above.txt', Buffer.from('out'), { folderId: top.id })

    const { token } = (await owner.post('/api/drive/shares', { folderId: shared.id, expires: '1d' })).json
    const root = await viewer.get(`/api/shares/${token}`)
    assert.deepEqual([root.json.share.kind, root.json.share.name, root.json.share.locked], ['folder', 'shared', false])
    assert.deepEqual(root.json.folders.map((f) => f.name), ['sub'])
    assert.deepEqual(root.json.path, [{ id: shared.id, name: 'shared' }])

    const deeper = await viewer.get(`/api/shares/${token}?folder=${sub.id}`)
    assert.deepEqual(deeper.json.path.map((p) => p.name), ['shared', 'sub'])
    assert.deepEqual(deeper.json.files.map((f) => f.name), ['inside.txt'])
    assert.equal((await viewer.binary('GET', `/api/shares/${token}/files/${inside.id}`)).data.toString(), 'in')

    assert.equal((await viewer.get(`/api/shares/${token}?folder=${top.id}`)).status, 404)
    assert.equal((await viewer.binary('GET', `/api/shares/${token}/files/${above.id}`)).status, 404)
    // Opening the link counts, going into its folders doesn't
    assert.equal((await owner.get('/api/drive/shares')).json.shares[0].views, 1)
  })

  test('gone with what it shows, or with its owner\'s drive', async () => {
    const { id: ownerId, client: owner } = await made('gina', { drive: true })
    const { client: viewer } = await made('walt')
    const kept = await upload(owner, 'kept', bytes(5))
    const removed = await upload(owner, 'removed', bytes(5))
    const keptLink = (await owner.post('/api/drive/shares', { fileId: kept.id, expires: '30d' })).json.token
    const removedLink = (await owner.post('/api/drive/shares', { fileId: removed.id, expires: '30d' })).json.token
    assert.equal((await owner.post('/api/drive/shares', { fileId: kept.id, expires: '2d' })).status, 400)
    assert.equal((await owner.post('/api/drive/shares', { fileId: kept.id, folderId: 1, expires: '1d' })).status, 400)

    await owner.delete(`/api/drive/files/${removed.id}`)
    await owner.delete(`/api/drive/trash/file/${removed.id}`)
    assert.equal((await viewer.get(`/api/shares/${removedLink}`)).status, 404)
    assert.equal((await owner.get('/api/drive/shares')).json.shares.length, 1)

    await admin.put(`/api/admin/users/${ownerId}/drive`, { enabled: false })
    assert.equal((await viewer.get(`/api/shares/${keptLink}`)).status, 404)
    await admin.put(`/api/admin/users/${ownerId}/drive`, { enabled: true })
    assert.equal((await viewer.get(`/api/shares/${keptLink}`)).status, 200)
  })
})

describe("the admin on an account's drive", () => {
  test('sees, adds and deletes its files and links (?user=)', async () => {
    const { id, client: owner } = await made('alma', { drive: true })
    const { client: other } = await made('otto', { drive: true })
    const folder = (await owner.post('/api/drive/folders', { parentId: null, name: 'mine' })).json
    const file = await upload(owner, 'secret.txt', Buffer.from('hers'), { folderId: folder.id })
    const share = (await owner.post('/api/drive/shares', { fileId: file.id, expires: '7d' })).json
    const as = (path) => `${path}${path.includes('?') ? '&' : '?'}user=${id}`

    // Nobody else
    assert.equal((await other.get(as('/api/drive'))).status, 403)
    assert.equal((await new Client().get(as('/api/drive'))).status, 401)
    assert.equal((await admin.get('/api/drive?user=999999')).status, 404)

    const root = await admin.get(as('/api/drive'))
    assert.deepEqual(root.json.folders.map((f) => f.name), ['mine'])
    assert.deepEqual(root.json.usage, { used: 4, quota: 100000, maxFileSize: 60000, chunkSize: 8 * 1024 * 1024, trashDays: 1 })
    assert.deepEqual((await admin.get(as(`/api/drive?folder=${folder.id}`))).json.files.map((f) => f.name), ['secret.txt'])
    assert.equal((await admin.binary('GET', as(`/api/drive/files/${file.id}`))).data.toString(), 'hers')
    assert.deepEqual((await admin.get(as('/api/drive/shares'))).json.shares.map((s) => s.token), [share.token])

    // Adds a file in pieces and a link, into the account's drive and against its quota
    const created = await admin.post(as('/api/drive/uploads'), { folderId: folder.id, name: 'from admin.txt', size: 3 })
    assert.equal(created.status, 201)
    assert.equal((await admin.binary('PUT', as(`/api/drive/uploads/${created.json.id}?offset=0`), Buffer.from('abc'))).status, 200)
    const added = await admin.post(as(`/api/drive/uploads/${created.json.id}/complete`))
    assert.equal(added.status, 201)
    assert.deepEqual((await owner.get(`/api/drive?folder=${folder.id}`)).json.files.map((f) => f.name), ['from admin.txt', 'secret.txt'])
    assert.equal((await owner.get('/api/drive')).json.usage.used, 7)
    const adminShare = await admin.post(as('/api/drive/shares'), { fileId: added.json.id, expires: 'never' })
    assert.equal(adminShare.status, 201)
    assert.equal((await owner.get('/api/drive/shares')).json.shares.length, 2)
    // Its link: the account's
    assert.equal((await other.get(`/api/shares/${adminShare.json.token}`)).json.share.owner, 'alma 名')

    // Deletes them
    assert.equal((await admin.delete(as(`/api/drive/shares/${share.id}`))).status, 200)
    assert.equal((await other.get(`/api/shares/${share.token}`)).status, 404)
    assert.equal((await admin.delete(as(`/api/drive/files/${file.id}`))).status, 200)
    assert.equal((await admin.delete(as(`/api/drive/folders/${folder.id}`))).status, 200)
    assert.equal((await owner.get('/api/drive')).json.folders.length, 0)
    // In the account's trash, which the admin empties
    assert.deepEqual((await admin.get(as('/api/drive/trash'))).json.trash.map((item) => item.name), ['mine', 'secret.txt'])
    assert.equal((await admin.delete(as('/api/drive/trash'))).status, 200)
    assert.deepEqual((await owner.get('/api/drive/trash')).json.trash, [])
    assert.equal((await owner.get('/api/drive/shares')).json.shares.length, 0)

    // Its drive off: still the admin's to look into
    await admin.put(`/api/admin/users/${id}/drive`, { enabled: false, quota: 100000 })
    assert.equal((await owner.get('/api/drive')).status, 403)
    assert.equal((await admin.get(as('/api/drive'))).status, 200)
  })
})

describe("the admin's page of drives", () => {
  test('every drive, how full, and the disk', async () => {
    const { id, client } = await made('dirk', { drive: true })
    await upload(client, 'kept.bin', bytes(300))
    const gone = await upload(client, 'gone.bin', bytes(200))
    await client.delete(`/api/drive/files/${gone.id}`)
    assert.equal((await client.get('/api/admin/drives')).status, 403)

    const drives = (await admin.get('/api/admin/drives')).json
    assert.ok(drives.disk.total > 0 && drives.disk.free > 0)
    assert.equal(drives.disk.minFree, 0)
    const dirk = drives.accounts.find((account) => account.id === id)
    assert.deepEqual(dirk, {
      id, username: 'dirk', displayName: 'dirk 名', role: 'user', enabled: true, quota: 100000, used: 500, files: 2, trashed: 200, trashDays: 1,
    })
    assert.ok(drives.quotas >= 100000 && drives.used >= 500)
  })
})

describe('the console', () => {
  test('lists the drives', () => {
    const out = server.console('drive list')
    assert.match(out, /author\s+0 MiB of 0 MiB, \d+ files\n/)
    assert.match(out, /fiona\s+0 MiB of 0 MiB, 2 files\n/)
    // Off, nothing in it
    assert.doesNotMatch(out, /nadia/)
  })
})

describe('deleting accounts', () => {
  test('takes their drive off the disk', async () => {
    const { id, client } = await made('della', { drive: true })
    const before = storedFiles().length
    await upload(client, 'a', bytes(10))
    await upload(client, 'b', bytes(10))
    assert.equal(storedFiles().length, before + 2)
    assert.equal((await admin.delete(`/api/admin/users/${id}`)).status, 200)
    assert.equal(storedFiles().length, before)
  })
})
