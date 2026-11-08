import { screen } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { http, HttpResponse } from 'msw'
import { renderPage } from '../../tests/render'
import { server } from '../../tests/msw'
import Register from './register'

async function fill(user: ReturnType<typeof userEvent.setup>, values: Record<string, string>) {
  for (const [label, value] of Object.entries(values)) await user.type(screen.getByLabelText(label), value)
}

test('validates on the client with the server codes', async () => {
  const user = userEvent.setup()
  renderPage('/en/register', [{ path: '/:lang/register', Component: Register }])

  await fill(user, { 'User name': 'a b', 'Display name': 'Alice', Email: 'nope', Password: 'short' })
  await user.click(screen.getByRole('button', { name: 'Sign up' }))

  expect(await screen.findAllByText('Invalid')).toHaveLength(2)
  expect(screen.getByText('Too short')).toBeInTheDocument()
})

test('puts the server field errors on their fields, then confirms', async () => {
  let attempt = 0
  server.use(
    http.post('*/api/auth/register', async ({ request }) => {
      const body = (await request.json()) as { locale: string }
      expect(body.locale).toBe('en')
      if (attempt++ === 0) return HttpResponse.json({ error: { code: 'conflict', fields: { username: 'taken' } } }, { status: 409 })
      return HttpResponse.json({ ok: true }, { status: 201 })
    }),
  )

  const user = userEvent.setup()
  renderPage('/en/register', [{ path: '/:lang/register', Component: Register }])
  await fill(user, { 'User name': 'alice', 'Display name': 'Alice', Email: 'alice@example.com', Password: 'long enough' })
  await user.click(screen.getByRole('button', { name: 'Sign up' }))

  expect(await screen.findByText('Taken already')).toBeInTheDocument()
  // A field error shows at the field only
  expect(screen.queryByText('That clashes with something that exists.')).not.toBeInTheDocument()

  await user.clear(screen.getByLabelText('User name'))
  await user.type(screen.getByLabelText('User name'), 'alice2')
  await user.click(screen.getByRole('button', { name: 'Sign up' }))
  expect(await screen.findByText(/We sent a mail to alice@example.com/)).toBeInTheDocument()
})
