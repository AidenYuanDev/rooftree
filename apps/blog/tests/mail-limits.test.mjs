// What keeps a script from having the server mail any address it likes: the picture captcha of
// registering, subscribing and password resets, and one mail per address per Blog.Auth.MailInterval.
// Two servers, one after the other: one with the captcha on (a script can't answer it, so only the
// refusals are tested here; tests/test_Captcha.cpp checks right answers), one with it off and a mail
// interval.

import assert from 'node:assert/strict'
import { after, before, describe, test } from 'node:test'
import { Client, lastMailTo, startServer } from './server.mjs'

// Mails are written by a worker, maybe after the response
async function waitForMail(outbox, address) {
  for (let i = 0; i < 100; ++i) {
    const mail = lastMailTo(outbox, address)
    if (mail)
      return mail
    await new Promise((r) => setTimeout(r, 20))
  }
  assert.fail(`no mail to ${address}`)
}

describe('captcha', () => {
  let server
  before(async () => { server = await startServer({ captcha: true }) })
  after(async () => {
    if (process.exitCode && server)
      console.log(server.log.join(''))
    await server?.stop()
  })

  test('a new picture every time, never cached', async () => {
    const client = new Client()
    const first = await client.get('/api/captcha')
    assert.equal(first.status, 200)
    assert.equal(first.headers.get('cache-control'), 'no-store')
    assert.equal(first.json.enabled, true)
    assert.match(first.json.id, /^[\w-]{24}$/)
    assert.equal(first.json.width, 160)
    assert.equal(first.json.height, 56)
    const png = Buffer.from(first.json.image.replace(/^data:image\/png;base64,/, ''), 'base64')
    assert.deepEqual([...png.subarray(0, 8)], [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])

    const second = await client.get('/api/captcha')
    assert.notEqual(second.json.id, first.json.id)
    assert.notEqual(second.json.image, first.json.image)
  })

  test('registering, subscribing and resets need it', async () => {
    const client = new Client()
    const requests = [
      ['/api/auth/register', { username: 'dave', displayName: 'Dave', email: 'dave@example.com', password: 'password for dave' }],
      ['/api/subscribe', { email: 'dave@example.com', lang: 'en' }],
      ['/api/auth/forgot-password', { email: 'author@example.com' }],
    ]
    for (const [path, body] of requests) {
      const missing = await client.post(path, body)
      assert.equal(missing.status, 400, path)
      assert.deepEqual(missing.json.error.fields, { captcha: 'required' }, path)

      const { json: captcha } = await client.get('/api/captcha')
      const wrong = await client.post(path, { ...body, captchaId: captcha.id, captcha: '!!!!!' })
      assert.deepEqual(wrong.json.error.fields, { captcha: 'wrong' }, path)

      const unknown = await client.post(path, { ...body, captchaId: 'made-up', captcha: 'ACDEF' })
      assert.deepEqual(unknown.json.error.fields, { captcha: 'wrong' }, path)
    }
    assert.equal(lastMailTo(server.outbox, 'dave@example.com'), undefined)
    assert.equal(lastMailTo(server.outbox, 'author@example.com'), undefined)
  })

  test('the other fields are checked first', async () => {
    const response = await new Client().post('/api/subscribe', { email: 'nope', lang: 'en' })
    assert.deepEqual(response.json.error.fields, { email: 'invalid' })
  })
})

describe('mail interval', () => {
  let server
  before(async () => { server = await startServer({ mailInterval: 60 }) })
  after(async () => {
    if (process.exitCode && server)
      console.log(server.log.join(''))
    await server?.stop()
  })

  test('one mail per address per interval', async () => {
    const client = new Client()
    assert.equal((await client.post('/api/subscribe', { email: 'erin@example.com', lang: 'en' })).status, 200)
    await waitForMail(server.outbox, 'erin@example.com')

    // Same address, any case: too soon, and when to come back
    const again = await client.post('/api/subscribe', { email: 'Erin@Example.com', lang: 'en' })
    assert.equal(again.status, 429)
    assert.equal(again.json.error.code, 'too_many_mails')
    const retryAfter = Number(again.headers.get('retry-after'))
    assert.ok(retryAfter > 50 && retryAfter <= 61, String(retryAfter))

    // Another address isn't held up
    assert.equal((await client.post('/api/subscribe', { email: 'frank@example.com', lang: 'en' })).status, 200)
  })

  test('the verification mail of a new account counts', async () => {
    const client = new Client()
    const registered = await client.post('/api/auth/register', { username: 'grace', displayName: 'Grace', email: 'grace@example.com', password: 'password for grace' })
    assert.equal(registered.status, 201)
    assert.equal((await client.post('/api/auth/login', { login: 'grace', password: 'password for grace' })).status, 200)

    const resend = await client.post('/api/auth/resend-verification')
    assert.equal(resend.status, 429)
    assert.ok(Number(resend.headers.get('retry-after')) > 50)
  })

  test('a reset too soon gets the same answer, without a mail', async () => {
    const client = new Client()
    assert.equal((await client.post('/api/auth/forgot-password', { email: 'author@example.com' })).status, 200)
    const first = await waitForMail(server.outbox, 'author@example.com')

    assert.equal((await client.post('/api/auth/forgot-password', { email: 'author@example.com' })).status, 200)
    await new Promise((r) => setTimeout(r, 200))
    assert.equal(lastMailTo(server.outbox, 'author@example.com').mail, first.mail)
  })
})
