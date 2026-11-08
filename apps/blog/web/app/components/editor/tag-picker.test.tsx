import { screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { http, HttpResponse } from 'msw'
import { useState } from 'react'
import { renderPage } from '../../../tests/render'
import { server } from '../../../tests/msw'
import { TagPicker } from './tag-picker'

const cpp = { slug: 'cpp', nameZh: '编程', nameEn: 'Programming', postCount: 2 }
const net = { slug: 'net', nameZh: '网络', nameEn: 'Network', postCount: 0 }

const me = (role: string) => ({
  user: {
    id: 1,
    username: 'alice',
    displayName: 'Alice',
    role,
    email: 'a@example.com',
    emailVerified: true,
    locale: 'en',
    totpEnabled: false,
  },
  csrfToken: 'csrf',
})

const submitted = vi.fn()
let picked = ['cpp']

function Page() {
  const [value, setValue] = useState<string[]>(['cpp'])
  return (
    <form
      onSubmit={(event) => {
        event.preventDefault()
        submitted()
      }}
    >
      <TagPicker
        value={value}
        onChange={(slugs) => {
          picked = slugs
          setValue(slugs)
        }}
      />
    </form>
  )
}

function serve(role: string) {
  const tags = [cpp, net]
  submitted.mockClear()
  picked = ['cpp']
  server.use(
    http.get('*/api/auth/me', () => HttpResponse.json(me(role))),
    http.get('*/api/tags', () => HttpResponse.json({ tags })),
    http.post('*/api/admin/tags', async ({ request }) => {
      const tag = { ...((await request.json()) as typeof cpp), postCount: 0 }
      tags.push(tag)
      return HttpResponse.json(tag, { status: 201 })
    }),
    http.patch('*/api/admin/tags/:slug', async ({ params, request }) => {
      const tag = tags.find((t) => t.slug === params.slug)!
      Object.assign(tag, await request.json())
      return HttpResponse.json(tag)
    }),
    http.delete('*/api/admin/tags/:slug', ({ params }) => {
      tags.splice(
        tags.findIndex((t) => t.slug === params.slug),
        1,
      )
      return HttpResponse.json({ ok: true })
    }),
  )
  renderPage('/en/write', [{ path: '/:lang/write', Component: Page }])
}

test('finds a tag and picks it with Enter, without submitting the post', async () => {
  serve('user')
  const user = userEvent.setup()
  await screen.findByRole('button', { name: 'Network' })

  await user.type(screen.getByLabelText('Search tags'), 'net{Enter}')

  await waitFor(() => expect(picked).toEqual(['cpp', 'net']))
  expect(submitted).not.toHaveBeenCalled()
  expect(screen.getByRole('button', { name: 'Programming' })).toHaveAttribute('aria-pressed', 'true')
})

test('someone who is not the admin cannot create or change tags', async () => {
  serve('user')
  await screen.findByRole('button', { name: 'Network' })
  expect(screen.queryByRole('button', { name: 'New tag' })).not.toBeInTheDocument()
  expect(screen.queryByRole('button', { name: 'Manage tags' })).not.toBeInTheDocument()
})

test('the admin creates a tag from what was searched, and it is picked', async () => {
  serve('admin')
  const user = userEvent.setup()
  await user.click(await screen.findByRole('button', { name: 'New tag' }))
  await user.click(screen.getByRole('button', { name: 'Cancel' }))

  await user.type(screen.getByLabelText('Search tags'), 'rust')
  expect(screen.getByText('No such tag.')).toBeInTheDocument()
  await user.click(screen.getByRole('button', { name: 'New tag' }))
  expect(screen.getByLabelText('Slug')).toHaveValue('rust')
  await user.click(screen.getByRole('button', { name: 'Add tag' }))

  await waitFor(() => expect(picked).toEqual(['cpp', 'rust']))
  expect(await screen.findByRole('button', { name: 'rust' })).toHaveAttribute('aria-pressed', 'true')
  expect(submitted).not.toHaveBeenCalled()
})

test('the admin renames a tag', async () => {
  serve('admin')
  const user = userEvent.setup()
  await user.click(await screen.findByRole('button', { name: 'Manage tags' }))
  await user.click(await screen.findByRole('button', { name: 'Network' }))

  await user.clear(screen.getByLabelText('English name'))
  await user.type(screen.getByLabelText('English name'), 'Networking')
  await user.click(screen.getByRole('button', { name: 'Save' }))

  expect(await screen.findByRole('button', { name: 'Networking' })).toBeInTheDocument()
  expect(picked).toEqual(['cpp'])
  expect(submitted).not.toHaveBeenCalled()
})

test('deleting a tag asks first, says how many posts it is on, and unpicks it', async () => {
  serve('admin')
  const user = userEvent.setup()
  await user.click(await screen.findByRole('button', { name: 'Manage tags' }))
  await user.click(await screen.findByRole('button', { name: 'Programming' }))

  await user.click(screen.getByRole('button', { name: 'Delete' }))
  expect(screen.getByRole('alert')).toHaveTextContent('Posts using this tag: 2')
  expect(picked).toEqual(['cpp'])

  await user.click(screen.getByRole('button', { name: 'Delete for good' }))
  await waitFor(() => expect(picked).toEqual([]))
  await waitFor(() => expect(screen.queryByRole('button', { name: 'Programming' })).not.toBeInTheDocument())
})
