import { screen } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { expect, it } from 'vitest'
import { renderPage } from '../../../tests/render'
import { Composer } from './composer'

function ComposerPage() {
  return (
    <Composer
      channelId="group:1"
      replyTo={null}
      onCancelReply={() => {}}
      editing={null}
      onCancelEdit={() => {}}
      pendingFile={null}
      onPendingFile={() => {}}
      candidates={[{ id: 2, username: 'pg_bob', displayName: '小博' }]}
    />
  )
}

it('suggests who an @ names, and typing right after choosing goes after the name', async () => {
  renderPage('/zh/chat', [{ path: '/zh/chat', Component: ComposerPage }])
  // No pause between keys: the first key after Enter comes before the next frame, like fast typing
  const user = userEvent.setup({ delay: null })
  const box = screen.getByPlaceholderText('输入消息… (Enter 发送, Shift+Enter 换行)')

  await user.type(box, '@pg_')
  expect(screen.getByRole('option', { name: /小博/ })).toBeTruthy()
  // Chosen with Enter, one key at once, a frame, the rest: the cursor was once put back before that
  // first key a frame late, the rest landing in front of it ("@pg_bob 天见明")
  await user.keyboard('{Enter}明')
  await new Promise((resolve) => requestAnimationFrame(resolve))
  await user.keyboard('天见')
  expect(box).toHaveProperty('value', '@pg_bob 明天见')
  expect(screen.queryByRole('option')).toBeNull()
})
