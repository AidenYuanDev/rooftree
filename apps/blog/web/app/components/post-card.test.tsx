import { screen } from '@testing-library/react'
import { renderPage } from '../../tests/render'
import type { PostSummary } from '~/lib/schemas'
import { PostCard } from './post-card'

const post: PostSummary = {
  id: 1,
  slug: 'hello',
  lang: 'zh',
  fallback: true,
  originalLang: 'zh',
  title: '你好',
  summary: '第一篇',
  cover: '',
  tags: [{ slug: 'cpp', name: 'C++' }],
  translations: ['zh'],
  commentCount: 2,
  publishedAt: '2026-10-20T10:00:00Z',
  updatedAt: '2026-10-20T10:00:00Z',
}

test('links the post and its tags in the page language, marks a fallback', () => {
  renderPage('/en', [{ path: '/:lang', Component: () => <PostCard post={post} /> }])
  expect(screen.getByRole('link', { name: '你好' })).toHaveAttribute('href', '/en/posts/hello')
  expect(screen.getByRole('link', { name: 'C++' })).toHaveAttribute('href', '/en/tags/cpp')
  expect(screen.getByText('中文')).toBeInTheDocument()
  expect(screen.getByText('2 comments')).toBeInTheDocument()
  expect(screen.getByText('October 20, 2026')).toBeInTheDocument()
})
