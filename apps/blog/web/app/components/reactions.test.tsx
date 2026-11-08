import { screen } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { http, HttpResponse } from 'msw'
import { renderPage } from '../../tests/render'
import { server } from '../../tests/msw'
import { Reactions } from './reactions'
import { Toaster } from './ui/sonner'

const me = (user: object | null) => http.get('*/api/auth/me', () => HttpResponse.json({ user, csrfToken: user ? 'csrf' : null }))
const reader = { id: 3, username: 'bob', displayName: 'bob', role: 'user', emailVerified: true }

function render() {
  const Page = () => (
    <>
      <Reactions target="post:7" />
      <Toaster />
    </>
  )
  return renderPage('/en/posts/7', [{ path: '/:lang/posts/:id', Component: Page }])
}

test('counts, and a click takes the own one back', async () => {
  const posted: unknown[] = []
  server.use(
    me(reader),
    http.get('*/api/reactions', ({ request }) => {
      expect(new URL(request.url).searchParams.get('targets')).toBe('post:7')
      return HttpResponse.json({ reactions: { 'post:7': { counts: { heart: 2, cat: 1 }, mine: ['heart'] } } })
    }),
    http.post('*/api/reactions', async ({ request }) => {
      posted.push(await request.json())
      return HttpResponse.json({ counts: { heart: 1, cat: 1 }, mine: [] })
    }),
  )
  const user = userEvent.setup()
  render()

  const heart = await screen.findByRole('button', { name: 'Love' })
  expect(heart).toHaveTextContent('2')
  expect(heart).toHaveAttribute('aria-pressed', 'true')
  expect(screen.getByRole('button', { name: 'Meow' })).toHaveAttribute('aria-pressed', 'false')

  await user.click(heart)
  await vi.waitFor(() => expect(posted).toEqual([{ target: 'post:7', kind: 'heart', on: false }]))
  expect(await screen.findByRole('button', { name: 'Love' })).toHaveTextContent('1')
})

test('without a login nothing is sent', async () => {
  const posted: unknown[] = []
  server.use(
    me(null),
    http.get('*/api/reactions', () => HttpResponse.json({ reactions: { 'post:7': { counts: { like: 1 }, mine: [] } } })),
    http.post('*/api/reactions', () => {
      posted.push(1)
      return HttpResponse.json({})
    }),
  )
  const user = userEvent.setup()
  render()
  await user.click(await screen.findByRole('button', { name: 'Like' }))
  expect(await screen.findByText('Log in to react.')).toBeInTheDocument()
  expect(posted).toEqual([])
})
