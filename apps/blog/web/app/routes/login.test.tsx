import { screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { http, HttpResponse } from 'msw'
import { renderPage } from '../../tests/render'
import { server } from '../../tests/msw'
import Login from './login'

const me = {
  user: {
    id: 1,
    username: 'alice',
    displayName: 'Alice',
    role: 'user',
    email: 'a@example.com',
    emailVerified: true,
    locale: 'en',
    totpEnabled: true,
  },
  csrfToken: 'csrf',
}

function Home() {
  return <p>home page</p>
}

test('asks for the TOTP code when the account has one, then logs in', async () => {
  const bodies: unknown[] = []
  server.use(
    http.post('*/api/auth/login', async ({ request }) => {
      const body = (await request.json()) as { totp?: string }
      bodies.push(body)
      if (!body.totp) return HttpResponse.json({ error: { code: 'totp_required' } }, { status: 401 })
      return HttpResponse.json(me)
    }),
  )

  const user = userEvent.setup()
  renderPage('/en/login', [
    { path: '/:lang/login', Component: Login },
    { path: '/:lang', Component: Home },
  ])

  await user.type(screen.getByLabelText('User name or email'), 'alice')
  await user.type(screen.getByLabelText('Password'), 'secret password')
  await user.click(screen.getByRole('button', { name: 'Log in' }))

  const code = await screen.findByLabelText('Verification code')
  // totp_required is a step, not an error to show
  expect(screen.queryByRole('alert')).not.toBeInTheDocument()

  await user.type(code, '123456')
  await user.click(screen.getByRole('button', { name: 'Log in' }))

  expect(await screen.findByText('home page')).toBeInTheDocument()
  expect(bodies).toEqual([
    { login: 'alice', password: 'secret password' },
    { login: 'alice', password: 'secret password', totp: '123456' },
  ])
})

test('shows a wrong password in the page language', async () => {
  server.use(http.post('*/api/auth/login', () => HttpResponse.json({ error: { code: 'invalid_credentials' } }, { status: 401 })))
  const user = userEvent.setup()
  renderPage('/zh/login', [{ path: '/:lang/login', Component: Login }])

  await user.type(screen.getByLabelText('用户名或邮箱'), 'alice')
  await user.type(screen.getByLabelText('密码'), 'wrong')
  await user.click(screen.getByRole('button', { name: '登录' }))

  expect(await screen.findByText('用户名或密码不对。')).toBeInTheDocument()
})

test('checks required fields before sending anything', async () => {
  const user = userEvent.setup()
  renderPage('/en/login', [{ path: '/:lang/login', Component: Login }])
  await user.click(screen.getByRole('button', { name: 'Log in' }))
  await waitFor(() => expect(screen.getAllByText('Required')).toHaveLength(2))
})
