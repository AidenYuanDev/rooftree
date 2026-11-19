import { join } from 'node:path'
import { type Browser, expect, type Page, test } from '@playwright/test'
import { loginAsAdmin } from './helpers'

// The chat page (routes/chat.tsx): replies, pictures, edits and recalls, @mentions, search, pinned
// chats, the owner's group management. EVIDENCE_DIR: where the screenshots are kept

test.describe.configure({ mode: 'serial' })

// The smallest PNG: one pixel
const png = Buffer.from(
  '89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d49444154789c6360000002000154a24f5d0000000049454e44ae426082',
  'hex',
)

async function evidence(page: Page, name: string) {
  if (process.env.EVIDENCE_DIR) await page.screenshot({ path: join(process.env.EVIDENCE_DIR, `chat-page-${name}.png`) })
}

async function csrfFetch(page: Page, path: string, init: { method: string; body?: unknown }) {
  return page.evaluate(
    async ({ path, init }) => {
      const me = (await (await fetch('/api/auth/me')).json()) as { csrfToken: string }
      const response = await fetch(path, {
        method: init.method,
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': me.csrfToken },
        body: init.body === undefined ? undefined : JSON.stringify(init.body),
      })
      return { status: response.status, json: (await response.json().catch(() => null)) as unknown }
    },
    { path, init },
  )
}

// A reader made by the admin, logged in in a browser of its own
async function reader(browser: Browser, admin: Page, username: string, displayName: string) {
  const made = await csrfFetch(admin, '/api/admin/users', {
    method: 'POST',
    body: { username, displayName, email: `${username}@example.com`, password: 'password1234', emailVerified: true },
  })
  expect(made.status).toBeLessThan(300)
  const context = await browser.newContext()
  const page = await context.newPage()
  await page.goto('/zh/login')
  await page.getByLabel('用户名或邮箱').fill(username)
  await page.getByLabel('密码').fill('password1234')
  await page.getByRole('button', { name: '登录' }).click()
  await expect(page.getByRole('button', { name: displayName })).toBeVisible()
  return page
}

async function befriend(a: Page, b: Page, bUsername: string) {
  expect((await csrfFetch(a, '/api/user/friends/requests', { method: 'POST', body: { to: bUsername } })).status).toBe(200)
  const requests = (await csrfFetch(b, '/api/user/friends/requests', { method: 'GET' })).json as { incoming: { id: number }[] }
  expect((await csrfFetch(b, `/api/user/friends/requests/${requests.incoming[0]!.id}/accept`, { method: 'POST' })).status).toBe(200)
}

const placeholder = '输入消息… (Enter 发送, Shift+Enter 换行)'
const bubble = (page: Page, text: string) => page.locator('.rounded-2xl').filter({ hasText: text })

let ann: Page
let bob: Page

test('the chat page: chats on the left, a reply, a picture, an edit and a recall', async ({ browser, page }) => {
  await loginAsAdmin(page, 'zh')
  ann = await reader(browser, page, 'pg_ann', '小安')
  bob = await reader(browser, page, 'pg_bob', '小博')
  await befriend(ann, bob, 'pg_bob')

  // From the dock to the page, with the chat it had open
  await ann.getByRole('button', { name: '实时聊天' }).click()
  await ann.getByRole('button', { name: /^小博/ }).click()
  await ann.getByRole('link', { name: '在聊天页打开' }).click()
  await expect(ann).toHaveURL(/\/zh\/chat\?c=dm%3A/)
  await expect(ann.getByRole('heading', { name: '聊天' })).toBeVisible()
  // No dock on the chat page
  await expect(ann.getByRole('button', { name: '实时聊天' })).toHaveCount(0)

  await ann.getByPlaceholder(placeholder).fill('你好，小博！')
  await ann.getByPlaceholder(placeholder).press('Enter')

  await bob.goto('/zh/chat')
  await bob.getByRole('button', { name: /^小安/ }).click()
  await expect(bubble(bob, '你好，小博！')).toBeVisible()

  // A reply quotes what it answers
  const first = bob.locator('.group').filter({ hasText: '你好，小博！' })
  await first.hover()
  await first.getByRole('button', { name: '回复' }).click()
  await expect(bob.getByText('回复 小安')).toBeVisible()
  await bob.getByPlaceholder(placeholder).fill('你好呀')
  await bob.getByPlaceholder(placeholder).press('Enter')
  const reply = ann.locator('.group').filter({ has: ann.locator('.rounded-2xl', { hasText: '你好呀' }) })
  await expect(reply.getByRole('button', { name: /小安: 你好，小博！/ })).toBeVisible()

  // A picture, shown to both
  await ann.getByTestId('chat-file-input').setInputFiles({ name: 'dot.png', mimeType: 'image/png', buffer: png })
  await expect(ann.getByRole('button', { name: '不发这个文件' })).toBeVisible()
  await ann.getByPlaceholder(placeholder).fill('一张图')
  await ann.getByPlaceholder(placeholder).press('Enter')
  await expect(bob.getByRole('img', { name: 'dot.png' })).toBeVisible()
  await expect(bubble(bob, '一张图')).toBeVisible()

  // Changed by its sender, everyone sees the change
  const typo = ann.locator('.group').filter({ has: ann.locator('.rounded-2xl', { hasText: '你好，小博！' }) })
  await typo.hover()
  await typo.getByRole('button', { name: '编辑' }).click()
  await expect(ann.getByText('编辑消息')).toBeVisible()
  await ann.getByPlaceholder(placeholder).fill('你好，小博！！')
  await ann.getByPlaceholder(placeholder).press('Enter')
  await expect(bubble(bob, '你好，小博！！')).toBeVisible()
  await expect(bob.getByText(/已编辑/).first()).toBeVisible()
  await evidence(bob, 'reply-picture-edit')

  // Taken back: gone for both, a line says so
  const picture = ann.locator('.group').filter({ has: ann.getByRole('img', { name: 'dot.png' }) })
  await picture.hover()
  await picture.getByRole('button', { name: '撤回' }).click()
  await expect(ann.getByText('你撤回了一条消息')).toBeVisible()
  await expect(bob.getByText('小安 撤回了一条消息')).toBeVisible()
  await expect(bob.getByRole('img', { name: 'dot.png' })).toHaveCount(0)
  await evidence(ann, 'recall')
})

test('a group: @mention suggestions and a toast, search goes to the message, pinned chats', async () => {
  await ann.getByRole('button', { name: '新建群聊' }).click()
  await ann.getByRole('menuitem', { name: '新建群聊' }).click()
  const dialog = ann.getByRole('dialog', { name: '新建群聊' })
  await dialog.getByPlaceholder('群聊名称').fill('读书角')
  await dialog.getByText('小博').click()
  await dialog.getByRole('button', { name: '新建群聊' }).click()
  await expect(ann).toHaveURL(/c=group%3A/)

  // Some messages to search through later
  const input = ann.getByPlaceholder(placeholder)
  for (const text of ['第一章读完了', '第二章有点难', '最喜欢的一句话在第三章']) await input.fill(text).then(() => input.press('Enter'))
  await expect(bubble(ann, '最喜欢的一句话在第三章')).toBeVisible()

  // Bob elsewhere: a toast when he is mentioned (once his page's WebSocket is there)
  await bob.goto('/zh/posts')
  await expect(bob.getByRole('button', { name: '实时聊天' })).toBeVisible()
  await bob.waitForTimeout(1000)
  await input.fill('@pg_')
  await expect(ann.getByRole('option', { name: /小博/ })).toBeVisible()
  await input.press('Enter')
  await expect(input).toHaveValue('@pg_bob ')
  await input.pressSequentially('明天一起讨论吗？')
  await input.press('Enter')
  await expect(bob.getByText('小安 提到了你')).toBeVisible()
  await evidence(bob, 'mention-toast')

  // The toast's link opens the group, where the mention stands out
  await bob.getByRole('link', { name: /读书角|打开聊天/ }).click()
  await expect(bob).toHaveURL(/\/zh\/chat\?c=group%3A/)
  await expect(bob.locator('.rounded-2xl').filter({ hasText: '明天一起讨论吗？' })).toHaveClass(/ring-amber/)

  // Search: the message found is gone to and marked
  await bob.getByRole('button', { name: '搜索聊天记录' }).first().click()
  await bob.getByRole('textbox', { name: '搜索聊天记录' }).fill('第二章')
  const result = bob.getByRole('button', { name: /第二章有点难/ })
  await expect(result).toBeVisible()
  await evidence(bob, 'search')
  await result.click()
  await expect(bob.locator('.group').filter({ hasText: '第二章有点难' })).toHaveClass(/bg-amber/)

  // Pinned: first in the list, marked
  await bob.getByRole('button', { name: '聊天设置' }).click()
  await bob.getByRole('menuitem', { name: '置顶' }).click()
  const list = bob.getByTestId('chat-channel-list')
  await expect(list.getByRole('button').first()).toContainText('读书角')
  await expect(list.getByRole('button').first().getByLabel('已置顶')).toBeAttached()
  await evidence(bob, 'pinned')
})

test('the owner renames the group, hands it over, and the new owner puts a member out', async () => {
  await ann.getByRole('button', { name: '聊天设置' }).click()
  await ann.getByRole('menuitem', { name: '聊天信息' }).click()
  await ann.getByRole('button', { name: '修改群名' }).click()
  await ann.getByRole('textbox', { name: '群聊名称' }).fill('周末读书角')
  await ann.getByRole('textbox', { name: '群聊名称' }).press('Enter')
  await expect(ann.getByText('群名已修改')).toBeVisible()
  await expect(bob.getByTestId('chat-channel-list').getByRole('button', { name: /^周末读书角/ })).toBeVisible()
  await evidence(ann, 'group-details')

  // Handed to bob: ann is a member like the others, bob may put her out
  await ann.getByRole('button', { name: '成员操作' }).click()
  await ann.getByRole('menuitem', { name: '转让群主' }).click()
  await ann.getByRole('alertdialog').getByRole('button', { name: '转让群主' }).click()
  await expect(ann.getByText('群主已转让')).toBeVisible()
  await expect(ann.getByRole('button', { name: '修改群名' })).toHaveCount(0)

  await bob.getByRole('button', { name: '聊天设置' }).click()
  await bob.getByRole('menuitem', { name: '聊天信息' }).click()
  await bob.getByRole('button', { name: '成员操作' }).click()
  await bob.getByRole('menuitem', { name: '移出群聊' }).click()
  await bob.getByRole('alertdialog').getByRole('button', { name: '移出群聊' }).click()
  await expect(bob.getByText('已移出群聊')).toBeVisible()
  await expect(ann.getByTestId('chat-channel-list').getByRole('button', { name: /^周末读书角/ })).toHaveCount(0)
})
