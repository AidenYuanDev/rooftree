import { screen, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { http, HttpResponse } from 'msw'
import { renderPage } from '../../tests/render'
import { server } from '../../tests/msw'
import { Comments } from './comments'

const author = (id: number, name: string, role: 'admin' | 'user' = 'user') => ({ id, username: name, displayName: name, role })
const comment = (id: number, body: string, extra: object = {}) => ({
  id,
  target: 'post:7',
  parentId: null,
  status: 'visible',
  body,
  author: author(2, 'alice'),
  replyTo: null,
  createdAt: '2026-10-20T10:00:00Z',
  editedAt: null,
  ...extra,
})

const page = {
  threads: [
    {
      ...comment(1, 'first thread'),
      replies: [
        comment(2, 'from the author', { parentId: 1, author: author(1, 'Author', 'admin') }),
        comment(3, 'thanks', { parentId: 1, replyTo: { id: 1, displayName: 'Author' } }),
      ],
    },
    { ...comment(4, ''), status: 'deleted', author: null, replies: [comment(5, 'orphan', { parentId: 4 })] },
  ],
  nextCursor: null,
}

const me = (user: object | null) => http.get('*/api/auth/me', () => HttpResponse.json({ user, csrfToken: user ? 'csrf' : null }))

function render() {
  return renderPage('/en/posts/x', [{ path: '/:lang/posts/:slug', Component: () => <Comments target="post:7" enabled /> }])
}

test('threads, replies to replies, deleted comments', async () => {
  server.use(
    me(null),
    http.get('*/api/comments', ({ request }) => {
      expect(new URL(request.url).searchParams.get('target')).toBe('post:7')
      return HttpResponse.json(page)
    }),
  )
  render()

  expect(await screen.findByText('first thread')).toBeInTheDocument()
  expect(screen.getByText('author')).toBeInTheDocument()
  expect(screen.getByText('@Author')).toBeInTheDocument()
  expect(screen.getByText('This comment was deleted.')).toBeInTheDocument()
  expect(screen.getByText('orphan')).toBeInTheDocument()
  expect(screen.getByRole('link', { name: 'Log in to comment.' })).toHaveAttribute('href', '/en/login?next=%2Fen%2Fposts%2Fx')
})

test('replying to a reply sends its id', async () => {
  const posted: unknown[] = []
  server.use(
    me({ id: 3, username: 'bob', displayName: 'bob', role: 'user', emailVerified: true }),
    http.get('*/api/comments', () => HttpResponse.json(page)),
    http.post('*/api/comments', async ({ request }) => {
      posted.push(await request.json())
      return HttpResponse.json(comment(9, 'ok', { parentId: 1 }), { status: 201 })
    }),
  )
  const user = userEvent.setup()
  render()

  const reply = (await screen.findByText('from the author')).closest('div')!.parentElement!
  await user.click(within(reply).getByRole('button', { name: 'Reply' }))
  const box = screen.getByLabelText('Reply to Author…')
  await user.type(box, 'me too')
  await user.click(within(box.closest('form')!).getByRole('button', { name: 'Reply' }))

  await vi.waitFor(() => expect(posted).toEqual([{ target: 'post:7', body: 'me too', replyTo: 2 }]))
})

test('an unconfirmed address can read but not write', async () => {
  server.use(
    me({ id: 3, username: 'bob', displayName: 'bob', role: 'user', emailVerified: false }),
    http.get('*/api/comments', () => HttpResponse.json(page)),
  )
  render()
  expect(await screen.findByText('Confirm your email address to comment.')).toBeInTheDocument()
  expect(screen.queryByRole('button', { name: 'Reply' })).not.toBeInTheDocument()
})

test('highlights new comments written by others while open, not initial or own ones', async () => {
  let current = page
  server.use(
    me({ id: 3, username: 'bob', displayName: 'bob', role: 'user', emailVerified: true }),
    http.get('*/api/comments', () => HttpResponse.json(current)),
  )
  const { queryClient } = render()
  expect((await screen.findByText('first thread')).closest('.comment-new')).toBeNull()

  current = {
    threads: [
      { ...comment(10, 'from bob', { author: author(3, 'bob') }), replies: [] },
      { ...comment(11, 'from carol', { author: author(4, 'carol') }), replies: [] },
      ...page.threads,
    ],
    nextCursor: null,
  }
  await queryClient.invalidateQueries({ queryKey: ['comments', 'post:7'] })

  expect((await screen.findByText('from carol')).closest('.comment-new')).not.toBeNull()
  expect(screen.getByText('from bob').closest('.comment-new')).toBeNull()
  expect(screen.getByText('first thread').closest('.comment-new')).toBeNull()
})
