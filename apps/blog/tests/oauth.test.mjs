// Logging in with GitHub and Google, against a fake of both: the token endpoint checks the client, the
// redirect URI and the PKCE verifier like the real ones, the user endpoints answer what a test put
// behind the code. The browser's trip to the provider is skipped: a test takes the state from the
// address the server gave and calls the callback with a code of its own.

import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { createServer } from 'node:http'
import { after, before, describe, test } from 'node:test'
import { adminPassword, baseUrl, Client, origin, startServer, totp } from './server.mjs'

const fakePort = 18190
const fakeUrl = `http://127.0.0.1:${fakePort}`

// code -> { provider, user, emails, challenge, redirectUri }; token -> the same
const codes = new Map()
const tokens = new Map()
// The users' pictures: a PNG, the addresses the server asked for
const picture = Buffer.concat([Buffer.from('89504e470d0a1a0a', 'hex'), Buffer.from('a provider picture')])
const pictureRequests = []
let fake
let server

function startFake() {
  fake = createServer(async (request, response) => {
    let body = ''
    for await (const chunk of request) body += chunk
    const [, provider, endpoint] = new URL(request.url, fakeUrl).pathname.split('/')
    const json = (status, value) => {
      response.writeHead(status, { 'Content-Type': 'application/json' })
      response.end(JSON.stringify(value))
    }

    if (endpoint === 'token') {
      const form = new URLSearchParams(body)
      const grant = codes.get(form.get('code'))
      codes.delete(form.get('code'))
      const verifier = form.get('code_verifier') ?? ''
      const challenge = createHash('sha256').update(verifier).digest('base64url')
      if (!grant || grant.provider !== provider || form.get('client_id') !== `${provider}-client` ||
        form.get('client_secret') !== `${provider}-secret` || form.get('redirect_uri') !== grant.redirectUri || challenge !== grant.challenge)
        return provider === 'github' ? json(200, { error: 'bad_verification_code' }) : json(400, { error: 'invalid_grant' })
      const token = `token-${Math.random()}`
      tokens.set(token, grant)
      return json(200, { access_token: token, token_type: 'bearer' })
    }

    if (endpoint === 'avatar') {
      pictureRequests.push(request.url)
      response.writeHead(200, { 'Content-Type': 'image/png' })
      return response.end(picture)
    }

    const grant = tokens.get((request.headers.authorization ?? '').replace(/^Bearer /, ''))
    if (!grant)
      return json(401, { message: 'Bad credentials' })
    if (endpoint === 'user')
      return json(200, grant.user)
    if (endpoint === 'emails')
      return json(200, grant.emails ?? [])
    json(404, {})
  })
  return new Promise((resolve) => fake.listen(fakePort, '127.0.0.1', resolve))
}

const github = (id, login, email, verified = true) => ({
  provider: 'github',
  user: { id, login, name: login.toUpperCase(), email: null, avatar_url: `${fakeUrl}/github/avatar/${id}?v=4` },
  emails: email ? [{ email: `other-${email}`, primary: false, verified }, { email, primary: true, verified }] : [],
})
const google = (sub, email, verified = true) => ({
  provider: 'google',
  user: { sub, email, email_verified: verified, name: 'Google User', picture: `${fakeUrl}/google/avatar/${sub}=s96-c` },
})

// Leaves for the provider and comes back with profile's code: the callback's response (a redirect).
// Logged in, the client asks for /api/auth/me then, like the page it lands on, for its CSRF token
async function oauth(client, profile, { link = false, next = '/en/posts', state } = {}) {
  const start = await client.post(`/api/auth/oauth/${profile.provider}`, { lang: 'en', next, link })
  assert.equal(start.status, 200, JSON.stringify(start.json))
  const url = new URL(start.json.url)
  assert.equal(url.origin + url.pathname, `${fakeUrl}/${profile.provider}/authorize`)
  assert.equal(url.searchParams.get('code_challenge_method'), 'S256')
  const code = `code-${Math.random()}`
  codes.set(code, { ...profile, challenge: url.searchParams.get('code_challenge'), redirectUri: url.searchParams.get('redirect_uri') })
  const response = await client.get(`/api/auth/oauth/${profile.provider}/callback?code=${code}&state=${state ?? url.searchParams.get('state')}`)
  await client.get('/api/auth/me')
  return response
}

function errorOf(response) {
  assert.equal(response.status, 302)
  return new URL(response.headers.get('location')).searchParams.get('oauth_error')
}

async function registerWithPassword(username) {
  const client = new Client()
  const email = `${username}@example.com`
  const password = `password for ${username}`
  assert.equal((await client.post('/api/auth/register', { username, displayName: username, email, password })).status, 201)
  assert.equal((await client.post('/api/auth/login', { login: username, password })).status, 200)
  return client
}

before(async () => {
  await startFake()
  server = await startServer({ oauth: fakeUrl })
})
after(async () => {
  if (process.exitCode && server)
    console.log(server.log.join(''))
  await server?.stop()
  fake?.close()
})

describe('logging in with GitHub and Google', () => {
  test('the providers that are set up', async () => {
    assert.deepEqual((await new Client().get('/api/auth/providers')).json, { providers: ['github', 'google'] })
    assert.equal((await new Client().post('/api/auth/oauth/gitlab', { lang: 'en' })).status, 404)
  })

  test('a new account from GitHub, logged in, back where it left', async () => {
    const client = new Client()
    const response = await oauth(client, github(101, 'octo-cat', 'octo@example.com'))
    assert.equal(response.status, 302)
    assert.equal(response.headers.get('location'), `${origin}/en/posts`)

    const { user } = (await client.get('/api/auth/me')).json
    assert.equal(user.username, 'octo_cat')
    assert.equal(user.displayName, 'OCTO-CAT')
    assert.equal(user.email, 'octo@example.com')
    assert.equal(user.emailVerified, true)
    assert.equal(user.hasPassword, false)
    assert.equal(user.locale, 'en')
    assert.deepEqual((await client.get('/api/auth/identities')).json.identities.map((i) => [i.provider, i.login]), [['github', 'octo-cat']])

    // Again: the same account
    const again = new Client()
    await oauth(again, github(101, 'octo-cat', 'octo@example.com'))
    assert.equal((await again.get('/api/auth/me')).json.user.id, user.id)
  })

  test('a taken user name gets a number', async () => {
    await registerWithPassword('dupe')
    const client = new Client()
    await oauth(client, google('g-dupe', 'dupe@gmail.example'))
    assert.match((await client.get('/api/auth/me')).json.user.username, /^dupe\d{4}$/)
  })

  test('an address with an account already is not merged', async () => {
    await registerWithPassword('heidi')
    const client = new Client()
    assert.equal(errorOf(await oauth(client, google('g-heidi', 'heidi@example.com'))), 'email_taken')
    assert.equal((await client.get('/api/auth/me')).json.user, null)
  })

  test('without a verified address no account is made', async () => {
    // GitHub: any verified address will do, not only the primary one
    const client = new Client()
    const anyVerified = github(108, 'secondary', 'secondary@example.com', false)
    anyVerified.emails[0].verified = true
    await oauth(client, anyVerified)
    assert.equal((await client.get('/api/auth/me')).json.user.email, 'other-secondary@example.com')

    assert.equal(errorOf(await oauth(new Client(), github(102, 'noaddress', null))), 'oauth_no_email')
    assert.equal(errorOf(await oauth(new Client(), github(103, 'unverified', 'unverified@example.com', false))), 'oauth_no_email')
    assert.equal(errorOf(await oauth(new Client(), google('g-unverified', 'g-unverified@example.com', false))), 'oauth_no_email')
  })

  test('linking on the account page, then logging in with it', async () => {
    const client = await registerWithPassword('ivan')
    const me = (await client.get('/api/auth/me')).json.user
    const linked = await oauth(client, google('g-ivan', 'ivan@gmail.example'), { link: true, next: '/en/account' })
    assert.equal(linked.headers.get('location'), `${origin}/en/account?oauth_linked=google`)
    assert.deepEqual((await client.get('/api/auth/identities')).json.identities.map((i) => i.provider), ['google'])

    const other = new Client()
    await oauth(other, google('g-ivan', 'ivan@gmail.example'))
    assert.equal((await other.get('/api/auth/me')).json.user.id, me.id)

    // Someone else's, or a second one of a provider
    const judy = await registerWithPassword('judy')
    assert.equal(errorOf(await oauth(judy, google('g-ivan', 'ivan@gmail.example'), { link: true })), 'identity_taken')
    assert.equal(errorOf(await oauth(client, google('g-ivan-2', 'ivan2@gmail.example'), { link: true })), 'already_linked')

    // Unlinking
    assert.equal((await client.request('DELETE', '/api/auth/identities/google')).status, 200)
    assert.deepEqual((await client.get('/api/auth/identities')).json.identities, [])
    assert.equal((await client.request('DELETE', '/api/auth/identities/google')).status, 404)
  })

  test('the only way in is not unlinked, until there is a password', async () => {
    const client = new Client()
    await oauth(client, github(104, 'karl', 'karl@example.com'))
    const unlink = await client.request('DELETE', '/api/auth/identities/github')
    assert.equal(unlink.status, 409)
    assert.equal(unlink.json.error.code, 'last_login_method')

    // The first password, without a current one
    const set = await client.post('/api/auth/change-password', { newPassword: 'a password for karl' })
    assert.equal(set.status, 200, JSON.stringify(set.json))
    assert.equal(set.json.user.hasPassword, true)
    assert.equal((await client.request('DELETE', '/api/auth/identities/github')).status, 200)
    assert.equal((await new Client().post('/api/auth/login', { login: 'karl', password: 'a password for karl' })).status, 200)

    // Now the current one is needed
    const change = await client.post('/api/auth/change-password', { newPassword: 'another password' })
    assert.equal(change.json.error.fields.currentPassword, 'required')
  })

  test('two-step login asks for the code after the provider', async () => {
    const client = new Client()
    await oauth(client, github(105, 'laura', 'laura@example.com'))
    const { secret } = (await client.post('/api/auth/totp/setup')).json
    assert.equal((await client.post('/api/auth/totp/enable', { code: totp(secret) })).status, 200)

    const other = new Client()
    const response = await oauth(other, github(105, 'laura', 'laura@example.com'))
    assert.equal(response.headers.get('location'), `${origin}/en/login?oauth=totp&provider=github`)
    assert.equal((await other.get('/api/auth/me')).json.user, null)

    assert.equal((await other.post('/api/auth/oauth-totp', { totp: '000000' })).json.error.code, 'totp_invalid')
    const login = await other.post('/api/auth/oauth-totp', { totp: totp(secret, 1) })
    assert.equal(login.status, 200, JSON.stringify(login.json))
    assert.equal(login.json.user.username, 'laura')
    assert.equal(login.json.next, '/en/posts')
    // The ticket is used up
    assert.equal((await other.post('/api/auth/oauth-totp', { totp: totp(secret, 1) })).json.error.code, 'oauth_expired')
    assert.equal((await new Client().post('/api/auth/oauth-totp', { totp: totp(secret, 1) })).json.error.code, 'oauth_expired')
  })

  test('a callback needs the state this browser left with', async () => {
    const client = new Client()
    assert.equal(errorOf(await oauth(client, github(106, 'mallory', 'mallory@example.com'), { state: 'made-up' })), 'oauth_state')

    // Someone else's callback link: the state isn't in this browser's cookie
    const start = await new Client().post('/api/auth/oauth/github', { lang: 'en' })
    const state = new URL(start.json.url).searchParams.get('state')
    const victim = new Client()
    assert.equal(errorOf(await victim.get(`/api/auth/oauth/github/callback?code=x&state=${state}`)), 'oauth_state')
    assert.equal((await victim.get('/api/auth/me')).json.user, null)
  })

  test('saying no at the provider, a bad code', async () => {
    const client = new Client()
    const start = await client.post('/api/auth/oauth/google', { lang: 'zh' })
    const state = new URL(start.json.url).searchParams.get('state')
    const denied = await client.get(`/api/auth/oauth/google/callback?error=access_denied&state=${state}`)
    assert.equal(denied.headers.get('location'), `${origin}/zh/login?oauth_error=oauth_cancelled&provider=google`)

    const again = await client.post('/api/auth/oauth/github', { lang: 'en' })
    const badCode = await client.get(`/api/auth/oauth/github/callback?code=unknown&state=${new URL(again.json.url).searchParams.get('state')}`)
    assert.equal(errorOf(badCode), 'oauth_failed')
  })

  test('only a page of ours to go back to', async () => {
    const client = new Client()
    const response = await oauth(client, github(107, 'nina', 'nina@example.com'), { next: '//evil.example/x' })
    assert.equal(response.headers.get('location'), `${origin}/en`)
  })

  test('a new account starts with the provider\'s picture', async () => {
    const client = new Client()
    await oauth(client, github(110, 'olga', 'olga@example.com'))
    const { avatar } = (await client.get('/api/auth/me')).json.user
    assert.match(avatar, /^\/uploads\/avatar-[A-Za-z0-9_-]+\.png$/)
    // Asked for at 256 pixels, served from here
    assert.equal(pictureRequests.at(-1), '/github/avatar/110?v=4&s=256')
    assert.deepEqual(Buffer.from(await (await fetch(`${baseUrl}${avatar}`)).arrayBuffer()), picture)
  })

  test('pictures: uploaded, the provider\'s, back to the initials', async () => {
    const client = new Client()
    await oauth(client, google('g-pat', 'pat@gmail.example'))
    const first = (await client.get('/api/auth/me')).json.user.avatar
    assert.equal(pictureRequests.at(-1), '/google/avatar/g-pat=s256-c')
    assert.equal((await client.get('/api/auth/identities')).json.identities[0].hasAvatar, true)

    assert.equal((await new Client().upload('/api/auth/avatar', picture)).status, 401)
    const gif = Buffer.from('GIF89a and the rest')
    assert.equal((await client.upload('/api/auth/avatar', gif)).json.error.fields.file, 'unsupported_type')
    const huge = Buffer.concat([picture, Buffer.alloc(512 * 1024)])
    assert.equal((await client.upload('/api/auth/avatar', huge)).json.error.fields.file, 'too_long')

    const webp = Buffer.concat([Buffer.from('RIFF\x10\x00\x00\x00WEBP', 'latin1'), Buffer.from('VP8 an encoded picture')])
    const uploaded = await client.upload('/api/auth/avatar', webp, 'me.webp', 'image/webp')
    assert.equal(uploaded.status, 200, JSON.stringify(uploaded.json))
    const second = uploaded.json.user.avatar
    assert.match(second, /^\/uploads\/avatar-[A-Za-z0-9_-]+\.webp$/)
    const served = await fetch(`${baseUrl}${second}`)
    assert.equal(served.headers.get('content-type'), 'image/webp')
    assert.deepEqual(Buffer.from(await served.arrayBuffer()), webp)
    // The previous file is gone
    assert.equal((await fetch(`${baseUrl}${first}`)).status, 404)

    const fromGoogle = await client.post('/api/auth/avatar/google')
    assert.equal(fromGoogle.status, 200, JSON.stringify(fromGoogle.json))
    assert.match(fromGoogle.json.user.avatar, /\.png$/)
    assert.equal((await client.post('/api/auth/avatar/github')).json.error.code, 'no_avatar')
    assert.equal((await client.post('/api/auth/avatar/other')).status, 404)

    const removed = await client.delete('/api/auth/avatar')
    assert.equal(removed.json.user.avatar, null)
    assert.equal((await fetch(`${baseUrl}${fromGoogle.json.user.avatar}`)).status, 404)
    assert.equal((await client.get('/api/auth/me')).json.user.avatar, null)
  })

  test('picture changes are limited per account, not per address', async () => {
    const client = new Client()
    await oauth(client, github(111, 'quinn', 'quinn@example.com'))
    for (let i = 0; i < 10; ++i)
      assert.equal((await client.delete('/api/auth/avatar')).status, 200)
    const limited = await client.delete('/api/auth/avatar')
    assert.equal(limited.status, 429)
    assert.equal(limited.json.error.code, 'too_many_avatar_changes')

    // Someone else from the same address isn't held back
    const other = new Client()
    await oauth(other, github(112, 'rosa', 'rosa@example.com'))
    assert.equal((await other.delete('/api/auth/avatar')).status, 200)
  })

  test('admins link a provider and still type their code after it', async () => {
    const admin = new Client()
    assert.equal((await admin.post('/api/auth/login', { login: 'author', password: adminPassword, totp: totp(server.adminTotpSecret) })).status, 200)
    const linked = await oauth(admin, github(109, 'author-gh', 'author-gh@example.com'), { link: true, next: '/en/account' })
    assert.equal(linked.headers.get('location'), `${origin}/en/account?oauth_linked=github`)

    const other = new Client()
    const response = await oauth(other, github(109, 'author-gh', 'author-gh@example.com'))
    assert.equal(response.headers.get('location'), `${origin}/en/login?oauth=totp&provider=github`)
    assert.equal((await other.get('/api/auth/me')).json.user, null)
    const login = await other.post('/api/auth/oauth-totp', { totp: totp(server.adminTotpSecret, 1) })
    assert.equal(login.status, 200, JSON.stringify(login.json))
    assert.equal(login.json.user.username, 'author')
    assert.equal(login.json.user.role, 'admin')

    // Two-step login is the admin's choice too: off, the password alone logs in
    assert.equal((await other.post('/api/auth/totp/disable', { password: adminPassword })).status, 200)
    assert.equal((await new Client().post('/api/auth/login', { login: 'author', password: adminPassword })).status, 200)
  })
})
