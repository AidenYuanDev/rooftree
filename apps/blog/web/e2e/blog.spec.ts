import { expect, type Page, test } from '@playwright/test'
import { loginAsAdmin, mailTo } from './helpers'

test.describe.configure({ mode: 'serial' })

// A CodeMirror field: all of its text replaced, as typed
async function replaceText(page: Page, label: string, text: string) {
  const field = page.getByRole('textbox', { name: label })
  await expect(field).toBeVisible()
  await field.click()
  await page.keyboard.press('ControlOrMeta+a')
  await page.keyboard.insertText(text)
}

test('the admin writes, publishes and renames a post', async ({ page }) => {
  await loginAsAdmin(page)

  await page.goto('/en/admin/tags')
  await page.getByLabel('Slug').fill('cpp')
  await page.getByLabel('Chinese name').fill('C++')
  await page.getByLabel('English name').fill('C++')
  await page.getByRole('button', { name: 'Add tag' }).click()
  await expect(page.getByText('cpp', { exact: true })).toBeVisible()

  await page.getByRole('navigation', { name: 'Admin' }).getByRole('link', { name: 'Posts' }).click()
  await page.getByRole('link', { name: 'New post' }).click()
  await page.getByLabel('Address').fill('hello-e2e')
  await page.getByRole('button', { name: 'C++' }).click()
  await page.getByLabel('Status').click()
  await page.getByRole('option', { name: 'Published' }).click()
  // The original language defaults to the page's: English here
  await page.getByLabel('Title').fill('Hello from the browser')
  await page.getByLabel('Summary').fill('Written in an end to end test')
  await replaceText(page, 'Text (Markdown)', '# Heading\n\nSome **bold** text.\n\n```cpp\nint main() {}\n```')
  await expect(page.locator('.cm-lp-h1')).toHaveText('Heading')
  await expect(page.locator('.cm-lp-strong')).toHaveText('bold')
  await page.getByRole('button', { name: 'Save', exact: true }).click()
  await expect(page.getByText('Saved.')).toBeVisible()
  await expect(page).toHaveURL(/\/en\/admin\/posts\/\d+$/)

  await page.goto('/en')
  await page.getByRole('link', { name: 'Hello from the browser' }).click()
  await expect(page.getByRole('heading', { level: 1, name: 'Hello from the browser' })).toBeVisible()
  await expect(page.locator('article strong')).toHaveText('bold')
  // Not prerendered yet (the dev server started before the post existed): rendered in the browser.
  // Prerendered pages are highlighted by Shiki, which deploy/rebuild.sh checks before a build goes live
  await expect(page.locator('article pre code')).toContainText('int main() {}')

  // The Chinese site has only what is written in Chinese
  await page.goto('/zh/posts/hello-e2e')
  await expect(page.getByText('这篇文章没有中文版。')).toBeVisible()

  // A new address keeps the old one working
  await page.goto('/en/admin')
  await page.getByRole('link', { name: 'Hello from the browser' }).click()
  await page.getByLabel('Address').fill('hello-renamed')
  await page.getByRole('button', { name: 'Save the settings' }).click()
  await expect(page.getByText('Saved.')).toBeVisible()
  await page.goto('/en/posts/hello-e2e')
  await expect(page).toHaveURL(/\/en\/posts\/hello-renamed$/)
  await expect(page.getByRole('heading', { level: 1, name: 'Hello from the browser' })).toBeVisible()
})

test('a reader registers, confirms the address and logs in', async ({ page }) => {
  await page.goto('/zh/register')
  await page.getByLabel('用户名').fill('reader')
  await page.getByLabel('昵称').fill('读者')
  await page.getByLabel('邮箱', { exact: true }).fill('reader@example.com')
  await page.getByLabel('密码').fill('a long password')
  await page.getByRole('button', { name: '注册' }).click()
  await expect(page.getByText('注册成功！')).toBeVisible()

  const mail = mailTo('reader@example.com')
  expect(mail?.text).toContain('/zh/verify-email?token=')
  await page.goto(`/zh/verify-email?token=${mail?.token}`)
  await expect(page.getByText('邮箱已确认，谢谢！')).toBeVisible()

  await page.goto('/zh/login')
  await page.getByLabel('用户名或邮箱').fill('reader@example.com')
  await page.getByLabel('密码').fill('a long password')
  await page.getByRole('button', { name: '登录' }).click()
  await expect(page.getByRole('button', { name: '读者' })).toBeVisible()
  await expect(page).toHaveURL(/\/zh$/)

  // Readers have no admin
  await page.goto('/zh/admin')
  await expect(page.getByText('只有管理员可以访问后台。')).toBeVisible()

  await page.getByRole('button', { name: '读者' }).click()
  await page.getByRole('menuitem', { name: '退出登录' }).click()
  await expect(page.getByText('已退出登录。')).toBeVisible()
  await expect(page).toHaveURL(/\/zh$/)
  await expect(page.getByRole('banner').getByRole('link', { name: '登录' })).toBeVisible()
})

test('the language switch keeps the page', async ({ page }) => {
  await page.goto('/zh/posts/hello-renamed')
  await page.getByRole('button', { name: '语言' }).click()
  await page.getByRole('menuitem', { name: 'English' }).click()
  await expect(page).toHaveURL(/\/en\/posts\/hello-renamed$/)
  await expect(page.getByRole('banner').getByRole('link', { name: 'Log in' })).toBeVisible()
})

test('unknown pages are a 404 in the right language', async ({ page }) => {
  await page.goto('/en/nothing/here')
  await expect(page.getByRole('heading', { name: 'Page not found' })).toBeVisible()
})

test('a reader comments, the admin hides it', async ({ page, browser }) => {
  await page.goto('/en/login')
  await page.getByLabel('User name or email').fill('reader')
  await page.getByLabel('Password').fill('a long password')
  await page.getByRole('button', { name: 'Log in' }).click()
  await expect(page.getByRole('button', { name: '读者' })).toBeVisible()

  await page.goto('/en/posts/hello-renamed')
  await page.getByLabel('Your thoughts…').fill('Great post!\nSecond line')
  await page.getByRole('button', { name: 'Post', exact: true }).click()
  await expect(page.getByText('Great post!')).toBeVisible()

  // The author answers in the thread
  const adminPage = await browser.newPage()
  await loginAsAdmin(adminPage)
  await adminPage.goto('/en/posts/hello-renamed')
  await adminPage.getByRole('button', { name: 'Reply' }).first().click()
  await adminPage.getByLabel('Reply to 读者…').fill('Thanks!')
  await adminPage
    .locator('form')
    .filter({ has: adminPage.getByLabel('Reply to 读者…') })
    .getByRole('button', { name: 'Reply' })
    .click()
  await expect(adminPage.getByText('Thanks!')).toBeVisible()

  await adminPage.goto('/en/admin/comments')
  await adminPage.getByRole('listitem').filter({ hasText: 'Great post!' }).getByRole('button', { name: 'Hide' }).click()
  await expect(adminPage.getByRole('listitem').filter({ hasText: 'Great post!' }).getByText('Hidden')).toBeVisible()

  // Readers don't see it anymore: a hidden comment that starts a thread hides the thread
  await page.reload()
  await expect(page.getByText('No comments yet, write the first one.')).toBeVisible()
  await expect(page.getByText('Great post!')).toHaveCount(0)
  await expect(page.getByText('Thanks!')).toHaveCount(0)
  await adminPage.close()
})

// Selects text of the post as a reader's drag would: from the first match of from to the end of to's
async function selectInPost(page: Page, from: string, to: string) {
  await page.locator('article .prose').getByText(from).first().waitFor()
  await page.evaluate(
    ({ from, to }) => {
      const body = document.querySelector('article .prose')!
      const walker = document.createTreeWalker(body, NodeFilter.SHOW_TEXT)
      const nodes: Text[] = []
      for (let node = walker.nextNode(); node; node = walker.nextNode()) nodes.push(node as Text)
      const start = nodes.find((node) => node.data.includes(from))!
      const end = nodes.find((node) => node.data.includes(to))!
      const range = document.createRange()
      range.setStart(start, start.data.indexOf(from))
      range.setEnd(end, end.data.indexOf(to) + to.length)
      const selection = window.getSelection()!
      selection.removeAllRanges()
      selection.addRange(range)
    },
    { from, to },
  )
}

const highlighted = (page: Page) =>
  page.evaluate(() => {
    const highlight = (CSS as unknown as { highlights: Map<string, Set<Range>> }).highlights.get('passage')
    return highlight ? [...highlight][0]!.toString() : null
  })

test('a reader comments on a passage, a reply chooses its own, the marker counts the replies', async ({ page, browser }) => {
  await page.goto('/en/login')
  await page.getByLabel('User name or email').fill('reader')
  await page.getByLabel('Password').fill('a long password')
  await page.getByRole('button', { name: 'Log in' }).click()
  await expect(page.getByRole('button', { name: '读者' })).toBeVisible()

  // Across the bold word, as the page shows it
  await page.goto('/en/posts/hello-renamed')
  await selectInPost(page, 'bold', ' text')
  await page.getByRole('button', { name: 'Comment on this' }).click()
  const panel = page.getByRole('complementary', { name: 'Comment on this passage' })
  await expect(panel.getByText('bold text')).toBeVisible()
  await panel.getByLabel('Your thoughts…').fill('Why bold?')
  await panel.getByRole('button', { name: 'Post', exact: true }).click()
  await expect(panel).toHaveCount(0)
  await expect(page.getByRole('button', { name: 'Discussion of this passage', exact: true })).toBeVisible()
  // The comment area quotes it too
  await expect(page.getByRole('button', { name: /On bold text/ })).toBeVisible()

  // The author answers about another passage
  const adminPage = await browser.newPage()
  await loginAsAdmin(adminPage)
  await adminPage.goto('/en/posts/hello-renamed')
  await adminPage.getByRole('button', { name: 'Discussion of this passage', exact: true }).click()
  const thread = adminPage.getByRole('complementary', { name: 'On this passage' })
  await expect(thread.getByText('Why bold?')).toBeVisible()
  await thread.getByText('Why bold?').click()
  expect(await highlighted(adminPage)).toBe('bold text')

  await thread.getByRole('button', { name: 'Reply' }).first().click()
  await expect(thread.getByText('On the passage it answers')).toBeVisible()
  await thread.getByRole('button', { name: 'Choose another' }).click()
  await expect(adminPage.getByText('Select the passage your reply is about in the post')).toBeVisible()
  await selectInPost(adminPage, 'int main', 'int main')
  await adminPage.getByRole('button', { name: 'Use for the reply' }).click()
  await expect(thread.getByText('int main')).toBeVisible()
  await thread.getByLabel('Reply to 读者…').fill('It is the code that matters')
  await thread.locator('form').getByRole('button', { name: 'Reply' }).click()
  await expect(thread.getByText('It is the code that matters')).toBeVisible()

  // Only the first comment's passage has the marker, with how many replies it has
  await expect(adminPage.getByRole('button', { name: 'Discussion of this passage, 1 reply' })).toBeVisible()
  await thread.getByText('It is the code that matters').click()
  expect(await highlighted(adminPage)).toBe('int main')
  await thread.getByText('Why bold?').click()
  expect(await highlighted(adminPage)).toBe('bold text')

  // A phone: the thread is a sheet from the bottom
  await adminPage.setViewportSize({ width: 390, height: 800 })
  const box = await thread.boundingBox()
  expect(Math.round(box!.y + box!.height)).toBe(800)
  expect(Math.round(box!.width)).toBe(390)
  await adminPage.keyboard.press('Escape')
  await expect(thread).toHaveCount(0)
  await adminPage.close()
})

test('a passage of the other language’s page: no marker here, its quote below leads to it', async ({ page }) => {
  await loginAsAdmin(page, 'zh')
  await page.evaluate(async () => {
    const me = (await (await fetch('/api/auth/me')).json()) as { csrfToken: string }
    const send = async (path: string, body: unknown) =>
      (await (
        await fetch(path, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': me.csrfToken },
          body: JSON.stringify(body),
        })
      ).json()) as { id: number }
    const post = await send('/api/admin/posts', {
      slug: 'two-languages',
      status: 'published',
      originalLang: 'en',
      translations: {
        en: { title: 'Two languages', body: `${'Filler.\n\n'.repeat(40)}A sentence in English.` },
        zh: { title: '两种语言', body: '一句中文。' },
      },
    })
    const anchor = { lang: 'en', block: 40, quote: 'sentence', prefix: 'A ', suffix: ' in English.', text: 'A sentence in English.' }
    await send('/api/comments', { target: `post:${post.id}`, body: 'About the English words', anchor })
  })

  await page.goto('/zh/posts/two-languages')
  await expect(page.getByText('About the English words')).toBeVisible()
  await expect(page.getByRole('button', { name: /^这段的讨论/ })).toHaveCount(0)
  await page.getByRole('link', { name: /引用 sentence 英文版/ }).click()
  await expect(page).toHaveURL(/\/en\/posts\/two-languages$/)
  const thread = page.getByRole('complementary', { name: 'On this passage' })
  await expect(thread.getByText('About the English words')).toBeVisible()
  await expect.poll(() => highlighted(page)).toBe('sentence')
  // Below the first screen: the page went to it
  await expect(page.getByText('A sentence in English.')).toBeInViewport()
})

test('the post loses a passage: the admin is told, its readers see what it was, the comment moves to another', async ({ page }) => {
  await loginAsAdmin(page)
  await page.goto('/en/admin')
  await page.getByRole('link', { name: 'Hello from the browser' }).click()
  // Its text is written live, published apart
  await replaceText(page, 'Text (Markdown)', '# Heading\n\nSomething else entirely.\n\n```cpp\nint main() {}\n```')
  await expect(page.getByText('Changes not published yet')).toBeVisible()
  await page.getByRole('button', { name: 'Publish the changes' }).click()
  await expect(page.getByText('A comment is on text the post no longer has')).toBeVisible()
  await page.getByRole('button', { name: 'Review' }).click()
  await expect(page).toHaveURL(/\/en\/admin\/comments\?passage=orphaned$/)
  const item = page.getByRole('listitem').filter({ hasText: 'Why bold?' })
  await expect(item.getByText('Text changed')).toBeVisible()
  await expect(item.getByText('Some bold text.')).toBeVisible()

  // Readers find it below the post, with what it was on
  await page.goto('/en/posts/hello-renamed')
  await page.getByText('A comment whose text changed').click()
  await expect(page.locator('mark', { hasText: 'bold text' })).toBeVisible()

  // The admin moves it to another passage
  await page.goto('/en/admin/comments?passage=orphaned')
  await item.getByRole('link', { name: 'Choose in the post' }).click()
  await expect(page.getByText('Select the passage this comment is on in the post')).toBeVisible()
  await selectInPost(page, 'Something else', 'Something else')
  await page.getByRole('button', { name: 'Move it here' }).click()
  await expect(page.getByText('Moved to the new passage')).toBeVisible()
  await expect(page.getByRole('button', { name: 'Discussion of this passage, 1 reply' })).toBeVisible()
  await expect(page.getByText('A comment whose text changed')).toHaveCount(0)

  // Its versions: as published, and as changed (a new address alone isn't one)
  await page.goto('/en/admin')
  await page.getByRole('link', { name: 'Hello from the browser' }).click()
  await page.getByRole('button', { name: 'Versions' }).click()
  await expect(page.getByRole('dialog').getByRole('listitem')).toHaveCount(2)
})

test('the admin invites an editor to a post and lets a reader write', async ({ page }) => {
  await loginAsAdmin(page)
  await page.goto('/en/admin')
  await page.getByRole('link', { name: 'Hello from the browser' }).click()
  await page.getByRole('button', { name: 'Editors' }).click()
  const dialog = page.getByRole('dialog')
  await expect(dialog.getByText('No other editors yet')).toBeVisible()
  await dialog.getByPlaceholder('User name', { exact: true }).fill('reader')
  await dialog.getByRole('button', { name: 'Publish' }).click()
  await dialog.getByRole('button', { name: 'Invite', exact: true }).click()
  await expect(page.getByText('reader is invited')).toBeVisible()
  const row = dialog.getByRole('listitem').filter({ hasText: '@reader' })
  await expect(row.getByRole('button', { name: 'Publish' })).toHaveAttribute('aria-pressed', 'true')
  await row.getByRole('button', { name: 'Settings' }).click()
  await expect(row.getByRole('button', { name: 'Settings' })).toHaveAttribute('aria-pressed', 'true')
  await row.getByRole('button', { name: 'Remove 读者' }).click()
  await expect(dialog.getByText('No other editors yet')).toBeVisible()
  await page.keyboard.press('Escape')

  await page.goto('/en/admin/users')
  await page.getByRole('link', { name: '读者' }).first().click()
  await page.getByLabel('Writes posts').click()
  await expect(page.getByText('It writes posts now')).toBeVisible()
  await expect(page.getByLabel('Writes posts')).toBeChecked()
})

test('two pages edit a post together, live', async ({ page, browser }) => {
  await loginAsAdmin(page)
  await page.goto('/en/admin')
  await page.getByRole('link', { name: 'Hello from the browser' }).click()
  await expect(page).toHaveURL(/\/en\/admin\/posts\/\d+$/)
  const url = page.url()
  const other = await browser.newPage()
  await loginAsAdmin(other)
  await other.goto(url)

  // One types, the other sees it, and where it is
  const summary = page.getByRole('textbox', { name: 'Summary' })
  await expect(summary).toContainText('Written in an end to end test')
  await summary.click()
  await page.keyboard.press('End')
  await page.keyboard.insertText(', together')
  const theirs = other.getByRole('textbox', { name: 'Summary' })
  await expect(theirs).toContainText('Written in an end to end test, together')
  await expect(other.locator('.cm-peer-label[data-name="The Author"]')).toBeVisible()

  // Both at once: neither loses what it typed
  await theirs.click()
  await other.keyboard.press('Home')
  await Promise.all([page.keyboard.insertText(' (mine)'), other.keyboard.insertText('[theirs] ')])
  for (const p of [page, other])
    await expect(p.getByRole('textbox', { name: 'Summary' })).toHaveText('[theirs] Written in an end to end test, together (mine)')
  await expect(page.getByText('Changes not published yet')).toBeVisible()

  // Gone and back: the page opens it as it is now
  await other.reload()
  await expect(other.getByRole('textbox', { name: 'Summary' })).toHaveText('[theirs] Written in an end to end test, together (mine)')
  await other.close()
  await page.getByRole('button', { name: 'Publish the changes' }).click()
  await expect(page.getByText('The changes are published')).toBeVisible()
  await expect(page.getByText('Readers get what is here.')).toBeVisible()

  // A version saved by hand, what it changed, and back to the one before
  const summary2 = page.getByRole('textbox', { name: 'Summary' })
  await summary2.click()
  await page.keyboard.press('End')
  await page.keyboard.insertText(' again')
  await page.getByRole('button', { name: 'Save a version' }).click()
  await page.getByRole('dialog').getByLabel('A note (optional)').fill('one more word')
  await page.getByRole('dialog').getByRole('button', { name: 'Save a version' }).click()
  await expect(page.getByText('Version saved')).toBeVisible()

  await page.getByRole('button', { name: 'Versions' }).click()
  const versions = page.getByRole('dialog')
  const saved = versions.getByRole('listitem').filter({ hasText: 'one more word' })
  await saved.getByRole('button').first().click()
  await expect(saved.locator('.bg-emerald-500\\/15', { hasText: 'again' })).toBeVisible()
  const published = versions.getByRole('listitem').nth(1)
  await published.getByRole('button').first().click()
  await published.getByRole('button', { name: 'Back to the working copy' }).click()
  await expect(page.getByText('It is the working copy again')).toBeVisible()
  await expect(page.getByRole('textbox', { name: 'Summary' })).toHaveText('[theirs] Written in an end to end test, together (mine)')
})

test('a reader the admin lets write publishes a post of its own, which names it', async ({ page }) => {
  await page.goto('/en/login')
  await page.getByLabel('User name or email').fill('reader')
  await page.getByLabel('Password').fill('a long password')
  await page.getByRole('button', { name: 'Log in' }).click()
  await page.getByRole('button', { name: '读者' }).click()
  await page.getByRole('menuitem', { name: 'Writing' }).click()
  await expect(page).toHaveURL(/\/en\/write$/)

  await page.getByRole('link', { name: 'New post' }).click()
  await page.getByLabel('Address').fill('by-a-reader')
  await page.getByLabel('Title').fill('A reader writes')
  await replaceText(page, 'Text (Markdown)', 'Written by a reader.')
  await page.getByLabel('Status').click()
  await page.getByRole('option', { name: 'Published' }).click()
  await page.getByRole('button', { name: 'Save', exact: true }).click()
  await expect(page).toHaveURL(/\/en\/write\/\d+$/)
  // Its own editor, live, with every right on it
  await expect(page.getByRole('textbox', { name: 'Text (Markdown)' })).toContainText('Written by a reader.')
  await expect(page.getByRole('button', { name: 'Editors' })).toBeVisible()

  await page.goto('/en/posts/by-a-reader')
  await page.getByRole('link', { name: '读者' }).last().click()
  await expect(page).toHaveURL(/\/en\/u\/reader$/)
  await expect(page.getByRole('heading', { level: 1, name: '读者' })).toBeVisible()
  await expect(page.getByRole('link', { name: 'A reader writes' })).toBeVisible()
})

test('search finds posts', async ({ page }) => {
  await page.goto('/en')
  // The command palette (⌘K, or the header's search button once hydrated)
  await expect(page.getByRole('link', { name: 'Log in' })).toBeVisible()
  await page.getByRole('button', { name: 'Search' }).click()
  await page.getByRole('combobox').fill('browser')
  await expect(page.getByRole('option', { name: 'Hello from the browser' })).toBeVisible()
  await page.getByRole('option', { name: /All results for/ }).click()
  await expect(page).toHaveURL(/\/en\/search\?q=browser$/)
  await expect(page.getByRole('link', { name: 'Hello from the browser' })).toBeVisible()
})

test("the footer's RSS and sitemap are pages for readers", async ({ page }) => {
  await page.goto('/en')
  await page.locator('footer').getByRole('link', { name: 'RSS' }).click()
  await expect(page).toHaveURL(/\/en\/rss$/)
  await expect(page.getByRole('main').locator('code').first()).toHaveText(/^https?:\/\/[^/]+\/en\/feed\.xml$/)
  await expect(page.getByRole('link', { name: 'Open in Feedly' }).first()).toHaveAttribute('href', /feedly\.com.*en%2Ffeed\.xml/)

  await page.locator('footer').getByRole('link', { name: 'Sitemap' }).click()
  await expect(page).toHaveURL(/\/en\/sitemap$/)
  await page.getByRole('main').getByRole('link', { name: 'Hello from the browser' }).click()
  await expect(page).toHaveURL(/\/en\/posts\//)
})

test("the status page is the admin's", async ({ page }) => {
  // Nowhere for readers to find it, and nothing in it for them
  await page.goto('/en')
  await expect(page.locator('footer').getByRole('link', { name: 'Status' })).toHaveCount(0)
  await page.goto('/en/status')
  await expect(page.getByText('Only the admin watches the clouds: log in with the admin account.')).toBeVisible()
  await expect(page.getByText('Up for')).toHaveCount(0)

  // The admin's menu has it
  await loginAsAdmin(page)
  await page.goto('/en')
  await page.getByRole('button', { name: 'The Author' }).click()
  await page.getByRole('menuitem', { name: 'Status' }).click()
  await expect(page).toHaveURL(/\/en\/status$/)
  await expect(page.getByText('Up for')).toBeVisible()
  // A sample a second in the tests: the day's charts are there
  await expect(page.getByRole('img', { name: /^Requests per minute: now/ })).toBeVisible()
  await expect(page.getByText(/^Updated \d\d:\d\d, every 10 seconds$/)).toBeVisible()
})

test('notes, thinking and the site settings', async ({ page }) => {
  await loginAsAdmin(page)

  await page.goto('/en/admin/notes')
  await page.getByRole('link', { name: 'New note' }).click()
  await page.getByLabel('Mood').fill('happy')
  await page.getByLabel('Weather').fill('clear')
  await page.getByLabel('Status').click()
  await page.getByRole('option', { name: 'Published' }).click()
  await page.getByLabel('Title').fill('Night on the roof')
  await replaceText(page, 'Text (Markdown)', 'Stars **everywhere**.')
  await page.getByRole('button', { name: 'Save', exact: true }).click()
  await expect(page.getByText('Saved.')).toBeVisible()
  await expect(page).toHaveURL(/\/en\/admin\/notes\/\d+$/)

  await page.goto('/en/admin/thinking')
  await page.getByPlaceholder("What's on your mind? (Markdown)").fill('Cats know everything.')
  await page.getByRole('button', { name: 'Publish' }).click()
  await expect(page.getByPlaceholder("What's on your mind? (Markdown)")).toHaveValue('')
  await expect(page.getByRole('main').getByText('Cats know everything.')).toBeVisible()

  await page.goto('/en/admin/site')
  await page.getByLabel('Site name · English').fill('Starry Cat')
  await page.getByLabel('Author').fill('The Author')
  await page.getByRole('button', { name: 'Save', exact: true }).click()
  await expect(page.getByText('Saved, the pages rebuild by themselves.')).toBeVisible()

  await page.goto('/en/notes')
  await expect(page.getByRole('banner').getByText('Starry Cat')).toBeVisible()
  await page.getByRole('link', { name: /Night on the roof/ }).click()
  await expect(page.getByRole('heading', { level: 1, name: 'Night on the roof' })).toBeVisible()
  await expect(page.getByText('Mood · happy')).toBeVisible()
  await expect(page.locator('article strong')).toHaveText('everywhere')

  await page.goto('/en/thinking')
  await expect(page.getByText('Cats know everything.')).toBeVisible()
  await page.goto('/en/timeline')
  await expect(page.getByRole('link', { name: /Night on the roof/ })).toBeVisible()
})

test('a reader signs the guestbook and applies for a link', async ({ page, browser }) => {
  await page.goto('/en/login')
  await page.getByLabel('User name or email').fill('reader')
  await page.getByLabel('Password').fill('a long password')
  await page.getByRole('button', { name: 'Log in' }).click()
  await expect(page.getByRole('button', { name: '读者' })).toBeVisible()

  await page.goto('/en/guestbook')
  await page.getByLabel('Your thoughts…').fill('Hello from the roof')
  await page.getByRole('button', { name: 'Post', exact: true }).click()
  await expect(page.getByText('Hello from the roof')).toBeVisible()

  await page.goto('/en/friends')
  await page.getByLabel('Name').fill('Reader site')
  await page.getByLabel('Address').fill('https://reader.example.com')
  await page.getByLabel('A word to the author').fill('Hi!')
  await page.getByRole('button', { name: 'Apply' }).click()
  await expect(page.getByText('Thanks! You will get a mail once it has been reviewed.')).toBeVisible()

  const adminPage = await browser.newPage()
  await loginAsAdmin(adminPage)
  await adminPage.goto('/en/admin/friends')
  await adminPage.getByRole('button', { name: 'Approve' }).click()
  await expect(adminPage.getByRole('button', { name: 'Approve' })).toHaveCount(0)
  // Sent after the response; in the reader's language (they registered on the Chinese pages), the site
  // has an English name only
  await expect
    .poll(() => mailTo('reader@example.com')?.text)
    .toMatch(/Reader site（https:\/\/reader\.example\.com）已经出现在Starry Cat的友链里了/)
  await adminPage.close()

  await page.goto('/en/friends')
  await expect(page.getByRole('link', { name: /Reader site/ })).toHaveAttribute('href', 'https://reader.example.com')
})

test('readers subscribe, confirm and unsubscribe', async ({ page }) => {
  // The admin's login may wait for the next TOTP step
  test.slow()
  await page.goto('/en')
  await expect(page.getByRole('link', { name: 'Log in' })).toBeVisible()
  await page.locator('footer').getByLabel('Email for updates').fill('fan@example.com')
  await page.locator('footer').getByRole('button', { name: 'Subscribe' }).click()
  await expect(page.locator('footer').getByText('Check your inbox')).toBeVisible()

  await expect.poll(() => mailTo('fan@example.com')?.token).toBeTruthy()
  await page.goto(`/en/subscribe/confirm?token=${mailTo('fan@example.com')?.token}`)
  await expect(page.getByText("You're subscribed")).toBeVisible()

  // A new post is mailed; its unsubscribe link works with a click (not by opening it)
  const adminPage = await page.context().browser()!.newPage()
  await loginAsAdmin(adminPage)
  await adminPage.goto('/en/admin/posts/new')
  await adminPage.getByLabel('Address').fill('mailed-e2e')
  await adminPage.getByLabel('Status').click()
  await adminPage.getByRole('option', { name: 'Published' }).click()
  await adminPage.getByLabel('Title').fill('A post for subscribers')
  await replaceText(adminPage, 'Text (Markdown)', 'Hello, subscribers.')
  await adminPage.getByRole('button', { name: 'Save', exact: true }).click()
  await expect(adminPage.getByText('Saved.')).toBeVisible()
  await adminPage.close()

  await expect.poll(() => mailTo('fan@example.com')?.text).toContain('A post for subscribers')
  const unsubscribe = mailTo('fan@example.com')!.text.match(/https?:\/\/[^/\s]+(\/en\/unsubscribe\?token=[\w-]+)/)![1]!
  await page.goto(unsubscribe)
  await expect(page.getByText("You won't get any more mails")).toBeVisible()
  await page.getByRole('button', { name: 'Unsubscribe' }).click()
  await expect(page.getByText("You're unsubscribed")).toBeVisible()
})

test('readers see how many read the same page', async ({ page, browser }) => {
  // Logging in (a new TOTP step) and waiting take longer than the usual 30 seconds
  test.setTimeout(60_000)
  // Readers, not tabs: a second tab of the same reader (same address, logged out) is no one more
  const sameReader = await browser.newPage()
  await page.goto('/en/posts/mailed-e2e')
  await sameReader.goto('/en/posts/mailed-e2e')
  await expect(page.locator('footer').getByText(/\d+ online/)).toBeVisible()
  await page.waitForTimeout(3000)
  await expect(page.getByText('2 reading this')).toHaveCount(0)
  await sameReader.close()

  // The author is someone else
  const author = await browser.newPage()
  await loginAsAdmin(author)
  await author.goto('/en/posts/mailed-e2e')
  await expect(page.getByText('2 reading this')).toBeVisible({ timeout: 10_000 })
  await author.close()
  await expect(page.getByText('2 reading this')).toHaveCount(0, { timeout: 10_000 })
})

test('the admin makes a moderator, takes a permission and sees them online', async ({ page, browser }) => {
  // The admin's login may wait for a new TOTP step, the last test took this one
  test.setTimeout(60_000)
  await loginAsAdmin(page)
  await page.goto('/en/admin/users')
  await page.getByRole('button', { name: 'New user' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByLabel('Username').fill('modmo')
  await dialog.getByLabel('Display name').fill('Mo the Moderator')
  await dialog.getByLabel('Email', { exact: true }).fill('mo@example.com')
  await dialog.getByLabel('Password (optional)').fill('password of mo')
  await dialog.getByLabel('Role').click()
  await page.getByRole('option', { name: 'Moderator' }).click()
  await dialog.getByRole('button', { name: 'New user' }).click()
  await expect(page).toHaveURL(/\/en\/admin\/users\/\d+$/)
  await expect(page.getByRole('heading', { name: 'Mo the Moderator' })).toBeVisible()

  await page.getByLabel('React', { exact: true }).click()
  await expect(page.getByText('No reactions')).toBeVisible()

  // The moderator has the comments and the users, nothing else
  const mo = await browser.newPage()
  await mo.goto('/en/login')
  await mo.getByLabel('User name or email').fill('modmo')
  await mo.getByLabel('Password', { exact: true }).fill('password of mo')
  await mo.getByRole('button', { name: 'Log in' }).click()
  await expect(mo.getByRole('button', { name: 'Mo the Moderator' })).toBeVisible()
  await mo.goto('/en/admin')
  await expect(mo).toHaveURL(/\/en\/admin\/comments$/)
  await expect(mo.getByRole('navigation', { name: 'Admin' }).getByRole('link')).toHaveText(['Comments', 'Users'])

  // Online, on the page they have open: the list is read again every 30 seconds, so until then, again
  const item = page.getByRole('listitem').filter({ hasText: 'Mo the Moderator' })
  await expect(async () => {
    await page.goto('/en/admin/users?status=online')
    await expect(item.getByText(/Online for/)).toBeVisible({ timeout: 2_000 })
  }).toPass({ timeout: 15_000 })
  await expect(item.getByText('/en/admin/comments')).toBeVisible()
  await page.screenshot({ path: 'test-results/admin-users.png', fullPage: true })
  await item.getByRole('link', { name: 'Mo the Moderator' }).click()
  await expect(page.getByRole('heading', { name: 'Mo the Moderator' })).toBeVisible()
  await expect(page.getByRole('button', { name: 'Log out everywhere' })).toBeVisible()
  await page.screenshot({ path: 'test-results/admin-user.png', fullPage: true })

  await mo.close()
  await page.goto('/en/admin/users?status=online')
  await expect(page.getByRole('listitem').filter({ hasText: 'Mo the Moderator' })).toHaveCount(0, { timeout: 10_000 })
})

test('the admin gives a reader a drive; they upload, share, and the link opens with its password', async ({ page, browser }) => {
  test.setTimeout(90_000)
  await loginAsAdmin(page)
  await page.goto('/en/admin/users')
  await page.getByRole('button', { name: 'New user' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByLabel('Username').fill('dana')
  await dialog.getByLabel('Display name').fill('Dana Drive')
  await dialog.getByLabel('Email', { exact: true }).fill('dana@example.com')
  await dialog.getByLabel('Password (optional)').fill('password of dana')
  await dialog.getByRole('button', { name: 'New user' }).click()
  await expect(page.getByRole('heading', { name: 'Dana Drive' })).toBeVisible()
  const danaAdminPage = new URL(page.url()).pathname
  // Every drive starts with no space: 1 GiB, then on
  await page.getByLabel('Size (GiB)').fill('1')
  await page.getByRole('button', { name: 'Save' }).last().click()
  await expect(page.getByText('Drive saved')).toBeVisible()
  await page.getByLabel('Has a drive').click()
  await expect(page.getByLabel('Has a drive')).toBeChecked()

  const dana = await browser.newPage()
  await dana.goto('/en/login')
  await dana.getByLabel('User name or email').fill('dana')
  await dana.getByLabel('Password', { exact: true }).fill('password of dana')
  await dana.getByRole('button', { name: 'Log in' }).click()
  await dana.getByRole('button', { name: 'Dana Drive' }).click()
  await dana.getByRole('menuitem', { name: 'Drive' }).click()
  await expect(dana).toHaveURL(/\/en\/drive$/)
  await expect(dana.getByText('This folder is empty.')).toBeVisible()

  await dana.getByRole('button', { name: 'New folder' }).click()
  await dana.getByLabel('Folder name').fill('Photos')
  await dana.getByRole('button', { name: 'Create' }).click()
  await dana.getByRole('link', { name: 'Photos' }).click()
  await expect(dana).toHaveURL(/\/en\/drive\?folder=\d+$/)
  // Uploads go where the page shows: into Photos once it is shown
  await expect(dana.getByRole('navigation', { name: 'breadcrumb' })).toContainText('Photos')
  const photos = new URL(dana.url()).pathname + new URL(dana.url()).search

  // A picture: 1×1, red
  const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==', 'base64')
  await dana.getByTestId('drive-upload').setInputFiles([
    { name: 'dot.png', mimeType: 'image/png', buffer: png },
    { name: 'notes.txt', mimeType: 'text/plain', buffer: Buffer.from('hello drive') },
  ])
  await expect(dana.getByRole('button', { name: /dot\.png/ })).toBeVisible()
  await expect(dana.getByRole('button', { name: /notes\.txt/ })).toBeVisible()
  await expect(dana.getByText(/ of 1 GB used/)).toBeVisible()
  await dana.screenshot({ path: 'test-results/drive.png', fullPage: true })

  await dana.getByRole('button', { name: /dot\.png/ }).click()
  await expect(dana.getByRole('dialog').getByRole('img', { name: 'dot.png' })).toBeVisible()
  await expect.poll(() => dana.getByRole('img', { name: 'dot.png' }).evaluate((img: HTMLImageElement) => img.naturalWidth)).toBe(1)
  await dana.keyboard.press('Escape')

  await dana.getByRole('listitem').filter({ hasText: 'dot.png' }).getByRole('button', { name: 'Actions' }).click()
  await dana.getByRole('menuitem', { name: 'Share' }).click()
  await dana.getByLabel('Password (optional)').fill('sesame')
  await dana.getByRole('button', { name: 'Create link' }).click()
  const link = await dana.getByRole('textbox', { name: 'Copy link' }).inputValue()
  expect(link).toMatch(/\/en\/s\/[\w-]{32}$/)
  await dana.keyboard.press('Escape')

  await page.goto(new URL(link).pathname)
  await expect(page.getByText('Shared by Dana Drive')).toBeVisible()
  await page.getByLabel('Password').fill('wrong')
  await page.getByRole('button', { name: 'Open' }).click()
  await expect(page.getByText('Wrong password.')).toBeVisible()
  await page.getByLabel('Password').fill('sesame')
  await page.getByRole('button', { name: 'Open' }).click()
  await expect(page.getByRole('img', { name: 'dot.png' })).toBeVisible()
  await expect(page.getByRole('link', { name: 'Download' })).toHaveAttribute('href', /\/api\/shares\/[\w-]+\/files\/\d+\?download=1$/)
  await page.screenshot({ path: 'test-results/share.png', fullPage: true })

  await dana.goto('/en/drive?tab=shares')
  await expect(dana.getByText('1 opened it · 1 views · 0 downloads')).toBeVisible()
  await dana.getByRole('button', { name: 'Who opened it' }).click()
  await expect(dana.getByRole('dialog').getByText('@author')).toBeVisible()
  await dana.keyboard.press('Escape')
  await dana.getByRole('button', { name: 'Stop sharing' }).click()
  await dana.getByRole('alertdialog').getByRole('button', { name: 'Stop sharing' }).click()
  await expect(dana.getByText("You haven't shared anything yet.")).toBeVisible()
  await page.reload()
  await expect(page.getByText("This link doesn't exist, or it was revoked.")).toBeVisible()

  // The admin looks into Dana's drive and deletes a file of it
  await page.goto(danaAdminPage)
  await page.getByRole('link', { name: 'Files and links' }).click()
  await expect(page.getByRole('heading', { name: "Dana Drive's drive" })).toBeVisible()
  await page.getByRole('link', { name: 'Photos' }).click()
  await expect(page.getByRole('navigation', { name: 'breadcrumb' })).toContainText('Photos')
  await page.getByRole('listitem').filter({ hasText: 'notes.txt' }).getByRole('button', { name: 'Actions' }).click()
  await page.getByRole('menuitem', { name: 'Delete' }).click()
  await page.getByRole('alertdialog').getByRole('button', { name: 'Delete' }).click()
  await expect(page.getByRole('button', { name: /notes\.txt/ })).toHaveCount(0)
  await expect(page.getByRole('button', { name: /dot\.png/ })).toBeVisible()
  await page.screenshot({ path: 'test-results/admin-user-drive.png', fullPage: true })
  await dana.goto(photos)
  await expect(dana.getByRole('button', { name: /dot\.png/ })).toBeVisible()
  await expect(dana.getByRole('button', { name: /notes\.txt/ })).toHaveCount(0)
  // The picture's small picture, drawn by Dana's browser when it was uploaded
  await expect(dana.getByRole('listitem').filter({ hasText: 'dot.png' }).locator('img')).toBeVisible()

  // In the trash, and back
  await dana.getByRole('tab', { name: 'Trash' }).click()
  await expect(dana.getByText('Was in: All files / Photos')).toBeVisible()
  await dana.getByRole('button', { name: 'Restore' }).click()
  await expect(dana.getByText('Restored “notes.txt”')).toBeVisible()
  await expect(dana.getByText('The trash is empty.')).toBeVisible()
  await dana.getByRole('tab', { name: 'Files' }).click()
  await expect(dana.getByRole('button', { name: /notes\.txt/ })).toBeVisible()

  // Both chosen, as one zip
  await dana.getByLabel('Choose all').check()
  await expect(dana.getByText('2 chosen')).toBeVisible()
  const download = dana.waitForEvent('download')
  await dana.getByRole('link', { name: 'Download', exact: true }).click()
  expect((await download).suggestedFilename()).toBe('Photos.zip')
  await dana.screenshot({ path: 'test-results/drive-chosen.png', fullPage: true })
  await dana.close()

  // Every drive at a glance
  await page.goto('/en/admin/drives')
  await expect(page.getByRole('row').filter({ hasText: '@dana' })).toContainText('2')
  await page.screenshot({ path: 'test-results/admin-drives.png', fullPage: true })
})

test('the admin sees what readers read and where they came from', async ({ page, browser }) => {
  // A reader's browser (a headless one counts as a bot)
  const context = await browser.newContext({
    userAgent: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0 Safari/537.36',
  })
  const reader = await context.newPage()
  await reader.goto('/en/timeline')
  await expect(reader.getByRole('heading', { name: 'Timeline' })).toBeVisible()
  await reader.getByRole('link', { name: 'Guestbook' }).first().click()
  await expect(reader).toHaveURL(/\/en\/guestbook$/)
  await context.close()

  await loginAsAdmin(page)
  await page.goto('/en/admin/stats')
  await expect(page.getByRole('heading', { name: 'Statistics' })).toBeVisible()
  await expect(page.getByRole('row').filter({ hasText: '/en/timeline' })).toBeVisible()
  await expect(page.getByRole('row').filter({ hasText: '/en/guestbook' })).toBeVisible()
  await page.screenshot({ path: 'test-results/admin-stats.png', fullPage: true })
})

test('an open post shows new comments, reactions and republished text without reloading', async ({ page, browser }) => {
  test.setTimeout(60_000)
  // A separate context so the reader isn't logged in as the admin
  const readerContext = await browser.newContext()
  const reader = await readerContext.newPage()
  await reader.goto('/en/posts/mailed-e2e')
  await expect(reader.getByRole('heading', { level: 1, name: 'A post for subscribers' })).toBeVisible()
  await expect(reader.locator('footer').getByText(/\d+ online/)).toBeVisible()

  await loginAsAdmin(page)
  await page.goto('/en/posts/mailed-e2e')
  await page.getByLabel('Your thoughts…').fill('Written while the post is open')
  await page.getByRole('button', { name: 'Post', exact: true }).click()
  await page.locator('article').getByRole('button', { name: 'React', exact: true }).click()
  await page.getByRole('menuitem', { name: 'Like' }).click()

  await expect(reader.getByText('Written while the post is open')).toBeVisible({ timeout: 10_000 })
  await expect(reader.locator('.comment-new')).toContainText('Written while the post is open')
  await expect(reader.locator('article').getByRole('button', { name: 'Like' })).toContainText('1', { timeout: 10_000 })

  await page.goto('/en/admin')
  await page.getByRole('link', { name: 'A post for subscribers' }).click()
  const body = page.getByRole('textbox', { name: 'Text (Markdown)' })
  await expect(body).toContainText('Hello, subscribers.')
  await body.click()
  await page.keyboard.press('Control+A')
  await page.keyboard.type('Updated while the reader is reading.')
  await page.getByRole('button', { name: 'Publish the changes' }).click()
  await expect(page.getByText('The changes are published')).toBeVisible()

  await expect(reader.locator('article')).toContainText('Updated while the reader is reading.', { timeout: 10_000 })
  await readerContext.close()
})

test('editors invite others and edit a post directly on its page', async ({ page, browser }) => {
  test.setTimeout(60_000)
  const danaContext = await browser.newContext()
  const dana = await danaContext.newPage()
  await dana.goto('/en/login')
  await dana.getByLabel('User name or email').fill('dana')
  await dana.getByLabel('Password', { exact: true }).fill('password of dana')
  await dana.getByRole('button', { name: 'Log in' }).click()
  await expect(dana.getByRole('button', { name: 'Dana Drive' })).toBeVisible()

  // Before being invited, Dana sees no edit or editors buttons on the post page
  await dana.goto('/en/posts/mailed-e2e')
  await expect(dana.getByRole('heading', { level: 1, name: 'A post for subscribers' })).toBeVisible()
  await expect(dana.locator('article').getByRole('button', { name: 'Edit', exact: true })).toHaveCount(0)
  await expect(dana.locator('article').getByRole('button', { name: 'Editors' })).toHaveCount(0)

  // The author invites Dana with publish right directly from the post page
  await loginAsAdmin(page)
  await page.goto('/en/posts/mailed-e2e')
  await page.locator('article').getByRole('button', { name: 'Editors' }).click()
  const dialog = page.getByRole('dialog', { name: 'Editors' })
  await dialog.getByLabel('Invite an editor').fill('dana')
  await dialog.locator('form').getByRole('button', { name: 'Publish' }).click()
  await dialog.getByRole('button', { name: 'Invite' }).click()
  await expect(page.getByText('dana is invited')).toBeVisible()
  await page.keyboard.press('Escape')

  // Dana's open page gets the Edit button live; Dana edits with Typora-like live preview and publishes
  await expect(dana.locator('article').getByRole('button', { name: 'Edit', exact: true })).toBeVisible({ timeout: 10_000 })
  await dana.locator('article').getByRole('button', { name: 'Edit', exact: true }).click()
  const body = dana.getByRole('textbox', { name: 'Text (Markdown)' })
  await expect(body).toContainText('Updated while the reader is reading.')
  await replaceText(dana, 'Text (Markdown)', '## Live heading\n\nEdited **directly** on the post page by Dana.')
  // Clicking the title moves the cursor off line 1 so "## " folds away like Typora
  await dana.getByRole('textbox', { name: 'Title' }).click()
  await expect(dana.locator('.cm-lp-h2')).toHaveText('Live heading')
  await expect(dana.locator('.cm-lp-strong')).toHaveText('directly')
  // Source mode shows the raw "## " again, and toggles back
  await dana.getByRole('button', { name: 'Source' }).click()
  await expect(body).toContainText('## Live heading')
  await dana.getByRole('button', { name: 'Live preview' }).click()
  await expect(dana.locator('.cm-lp-h2')).toHaveText('Live heading')

  await dana.getByRole('button', { name: 'Publish the changes' }).click()
  await expect(dana.getByText('The changes are published')).toBeVisible()
  await dana.getByRole('button', { name: 'Done' }).click()

  await expect(dana.locator('article h2')).toHaveText('Live heading')
  await expect(dana.locator('article strong')).toHaveText('directly')
  await danaContext.close()
})

test('a writer creates a post directly from the posts page, invites an editor on the draft and publishes', async ({ browser }) => {
  test.setTimeout(60_000)
  const writerContext = await browser.newContext()
  const writer = await writerContext.newPage()
  await writer.goto('/en/login')
  await writer.getByLabel('User name or email').fill('reader')
  await writer.getByLabel('Password', { exact: true }).fill('a long password')
  await writer.getByRole('button', { name: 'Log in' }).click()
  await expect(writer.getByRole('button', { name: '读者' })).toBeVisible()

  await writer.goto('/en/posts')
  await writer.getByRole('button', { name: 'New post' }).click()
  await writer.getByLabel('Address').fill('from-posts-page')
  await writer.getByLabel('Title').fill('Written from Posts')
  await replaceText(writer, 'Text (Markdown)', 'First line with **bold** words.\n\nSecond line where the cursor sits.')
  // Line 1 is not the cursor line so ** folds away; clicking line 1 unfolds ** across the whole line
  await expect(writer.locator('.cm-line').first()).toHaveText('First line with bold words.')
  await writer
    .locator('.cm-line')
    .first()
    .click({ position: { x: 10, y: 8 } })
  await expect(writer.locator('.cm-line').first()).toHaveText('First line with **bold** words.')

  await writer.getByRole('button', { name: 'Save draft' }).click()
  await expect(writer.getByText('Saved as a draft')).toBeVisible()
  await writer.getByRole('button', { name: 'Editors' }).click()
  const dialog = writer.getByRole('dialog', { name: 'Editors' })
  await dialog.getByLabel('Invite an editor').fill('dana')
  await dialog.getByRole('button', { name: 'Invite' }).click()
  await expect(writer.getByText('dana is invited')).toBeVisible()
  await writer.keyboard.press('Escape')

  await writer.getByRole('button', { name: 'Publish' }).click()
  await expect(writer).toHaveURL(/\/en\/posts\/from-posts-page$/)
  await expect(writer.getByRole('heading', { level: 1, name: 'Written from Posts' })).toBeVisible()
  await expect(writer.locator('article strong')).toHaveText('bold')
  await writerContext.close()
})

test('readers become friends: send request from profile, accept in account, and unfriend', async ({ browser }) => {
  // Create user fa
  const ctxA = await browser.newContext()
  const pageA = await ctxA.newPage()
  await pageA.goto('/zh/register')
  await pageA.getByLabel('用户名').fill('fa_user')
  await pageA.getByLabel('昵称').fill('小明')
  await pageA.getByLabel('邮箱', { exact: true }).fill('fa@example.com')
  await pageA.getByLabel('密码').fill('password1234')
  await pageA.getByRole('button', { name: '注册' }).click()
  await expect(pageA.getByText('注册成功！')).toBeVisible()
  const mailA = mailTo('fa@example.com')
  await pageA.goto(`/zh/verify-email?token=${mailA?.token}`)
  await expect(pageA.getByText('邮箱已确认，谢谢！')).toBeVisible()
  await pageA.goto('/zh/login')
  await pageA.getByLabel('用户名或邮箱').fill('fa_user')
  await pageA.getByLabel('密码').fill('password1234')
  await pageA.getByRole('button', { name: '登录' }).click()
  await expect(pageA.getByRole('button', { name: '小明' })).toBeVisible()

  // Create user fb
  const ctxB = await browser.newContext()
  const pageB = await ctxB.newPage()
  await pageB.goto('/zh/register')
  await pageB.getByLabel('用户名').fill('fb_user')
  await pageB.getByLabel('昵称').fill('小红')
  await pageB.getByLabel('邮箱', { exact: true }).fill('fb@example.com')
  await pageB.getByLabel('密码').fill('password1234')
  await pageB.getByRole('button', { name: '注册' }).click()
  await expect(pageB.getByText('注册成功！')).toBeVisible()
  const mailB = mailTo('fb@example.com')
  await pageB.goto(`/zh/verify-email?token=${mailB?.token}`)
  await expect(pageB.getByText('邮箱已确认，谢谢！')).toBeVisible()
  await pageB.goto('/zh/login')
  await pageB.getByLabel('用户名或邮箱').fill('fb_user')
  await pageB.getByLabel('密码').fill('password1234')
  await pageB.getByRole('button', { name: '登录' }).click()
  await expect(pageB.getByRole('button', { name: '小红' })).toBeVisible()

  // 小明访问小红的主页并发送好友申请
  await pageA.goto('/zh/u/fb_user')
  await expect(pageA.getByRole('heading', { level: 1, name: '小红' })).toBeVisible()
  await pageA.getByRole('button', { name: '加为好友' }).click()
  const dialog = pageA.getByRole('dialog')
  await dialog.getByPlaceholder('打个招呼吧（可选，最多 200 字）').fill('你好，交个朋友！')
  await dialog.getByRole('button', { name: '加为好友' }).click()
  await expect(pageA.getByText('好友申请已发送')).toBeVisible()
  await expect(pageA.getByRole('button', { name: '取消申请' })).toBeVisible()

  // 小红在账号页查看好友申请
  await pageB.goto('/zh/account')
  await pageB.getByRole('tab', { name: /好友申请/ }).click()
  await expect(pageB.getByText('@fa_user')).toBeVisible()
  await expect(pageB.getByText('"你好，交个朋友！"')).toBeVisible()

  // 小红同意申请
  await pageB.getByRole('button', { name: '同意' }).click()
  await expect(pageB.getByText('已添加为好友')).toBeVisible()

  // 小红的好友列表看到小明
  await pageB.getByRole('tab', { name: /好友列表/ }).click()
  await expect(pageB.getByText('@fa_user')).toBeVisible()

  // 小明刷新小红主页显示已是好友
  await pageA.goto('/zh/u/fb_user')
  await expect(pageA.getByText('已是好友')).toBeVisible()

  // 解除好友
  await pageA.goto('/zh/account')
  await expect(pageA.getByText('@fb_user')).toBeVisible()
  await pageA.getByTitle('解除好友').click()
  await pageA.getByRole('alertdialog').getByRole('button', { name: '解除好友' }).click()
  await expect(pageA.getByText('已解除好友关系')).toBeVisible()
  await expect(pageA.getByText('还没有好友。')).toBeVisible()

  await ctxA.close()
  await ctxB.close()
})

test('readers chat in square and direct messages via floating dock', async ({ browser }) => {
  // Create user ca
  const ctxA = await browser.newContext()
  const pageA = await ctxA.newPage()
  await pageA.goto('/zh/register')
  await pageA.getByLabel('用户名').fill('ca_user')
  await pageA.getByLabel('昵称').fill('小李')
  await pageA.getByLabel('邮箱', { exact: true }).fill('ca@example.com')
  await pageA.getByLabel('密码').fill('password1234')
  await pageA.getByRole('button', { name: '注册' }).click()
  await expect(pageA.getByText('注册成功！')).toBeVisible()
  const mailA = mailTo('ca@example.com')
  await pageA.goto(`/zh/verify-email?token=${mailA?.token}`)
  await expect(pageA.getByText('邮箱已确认，谢谢！')).toBeVisible()
  await pageA.goto('/zh/login')
  await pageA.getByLabel('用户名或邮箱').fill('ca_user')
  await pageA.getByLabel('密码').fill('password1234')
  await pageA.getByRole('button', { name: '登录' }).click()
  await expect(pageA.getByRole('button', { name: '小李' })).toBeVisible()

  // Create user cb
  const ctxB = await browser.newContext()
  const pageB = await ctxB.newPage()
  await pageB.goto('/zh/register')
  await pageB.getByLabel('用户名').fill('cb_user')
  await pageB.getByLabel('昵称').fill('小张')
  await pageB.getByLabel('邮箱', { exact: true }).fill('cb@example.com')
  await pageB.getByLabel('密码').fill('password1234')
  await pageB.getByRole('button', { name: '注册' }).click()
  await expect(pageB.getByText('注册成功！')).toBeVisible()
  const mailB = mailTo('cb@example.com')
  await pageB.goto(`/zh/verify-email?token=${mailB?.token}`)
  await expect(pageB.getByText('邮箱已确认，谢谢！')).toBeVisible()
  await pageB.goto('/zh/login')
  await pageB.getByLabel('用户名或邮箱').fill('cb_user')
  await pageB.getByLabel('密码').fill('password1234')
  await pageB.getByRole('button', { name: '登录' }).click()
  await expect(pageB.getByRole('button', { name: '小张' })).toBeVisible()

  // Make them friends
  await pageA.goto('/zh/u/cb_user')
  await pageA.getByRole('button', { name: '加为好友' }).click()
  const dialog = pageA.getByRole('dialog')
  await dialog.getByPlaceholder('打个招呼吧（可选，最多 200 字）').fill('我们做朋友吧')
  await dialog.getByRole('button', { name: '加为好友' }).click()
  await expect(pageA.getByText('好友申请已发送')).toBeVisible()

  await pageB.goto('/zh/account')
  await pageB.getByRole('tab', { name: /好友申请/ }).click()
  await pageB.getByRole('button', { name: '同意' }).click()
  await expect(pageB.getByText('已添加为好友')).toBeVisible()

  // Floating dock: 小李 opens chat dock and goes to square
  await pageA.goto('/zh')
  const dockBtnA = pageA.getByRole('button', { name: '实时聊天' })
  await expect(dockBtnA).toBeVisible()
  await dockBtnA.click()

  await expect(pageA.getByText('小屋广场')).toBeVisible()
  await pageA.getByText('小屋广场').click()

  const inputA = pageA.getByPlaceholder('输入消息… (Enter 发送, Shift+Enter 换行)')
  await inputA.fill('广场的大家大家好！')
  await inputA.press('Enter')
  await expect(pageA.locator('.rounded-2xl').filter({ hasText: '广场的大家大家好！' })).toBeVisible()

  // 小张 also opens square in dock and sees message
  await pageB.goto('/zh')
  const dockBtnB = pageB.getByRole('button', { name: '实时聊天' })
  await dockBtnB.click()
  await pageB.getByText('小屋广场').click()
  await expect(pageB.locator('.rounded-2xl').filter({ hasText: '广场的大家大家好！' })).toBeVisible()

  // Direct chat: 小李 returns to channel list, clicks 小张
  await pageA
    .locator('button')
    .filter({ has: pageA.locator('.lucide-arrow-left') })
    .click()
  const chanBtnB = pageA.getByRole('button', { name: /^小张/ })
  await expect(chanBtnB).toBeVisible()
  await chanBtnB.click()

  const dmInputA = pageA.getByPlaceholder('输入消息… (Enter 发送, Shift+Enter 换行)')
  await dmInputA.fill('小张，这是私聊！')
  await dmInputA.press('Enter')
  await expect(pageA.locator('.rounded-2xl').filter({ hasText: '小张，这是私聊！' })).toBeVisible()

  // 小张 returns and clicks 小李 to see DM
  await pageB
    .locator('button')
    .filter({ has: pageB.locator('.lucide-arrow-left') })
    .click()
  const chanBtnA = pageB.getByRole('button', { name: /^小李/ })
  await expect(chanBtnA).toBeVisible()
  await chanBtnA.click()
  await expect(pageB.locator('.rounded-2xl').filter({ hasText: '小张，这是私聊！' })).toBeVisible()

  // Group chat creation in dock: 小李 returns to channel list, clicks "+", chooses "新建群聊"
  await pageA
    .locator('button')
    .filter({ has: pageA.locator('.lucide-arrow-left') })
    .click()
  await pageA.getByTitle('新建群聊').click()
  await pageA.getByRole('menuitem', { name: '新建群聊' }).click()
  const grpDialog = pageA.getByRole('dialog', { name: '新建群聊' })
  await grpDialog.getByPlaceholder('群聊名称').fill('读书会群')
  await grpDialog.getByText('小张').click()
  await grpDialog.getByRole('button', { name: '新建群聊' }).click()
  await expect(pageA.getByText('群聊创建成功')).toBeVisible()

  // 小李 sends message in group
  const grpInputA = pageA.getByPlaceholder('输入消息… (Enter 发送, Shift+Enter 换行)')
  await grpInputA.fill('欢迎加入读书会！')
  await grpInputA.press('Enter')
  await expect(pageA.locator('.rounded-2xl').filter({ hasText: '欢迎加入读书会！' })).toBeVisible()

  // 小张 returns to channels, sees "读书会群", clicks and reads message
  await pageB
    .locator('button')
    .filter({ has: pageB.locator('.lucide-arrow-left') })
    .click()
  await expect(pageB.getByText('读书会群')).toBeVisible()
  await pageB.getByText('读书会群').click()
  await expect(pageB.locator('.rounded-2xl').filter({ hasText: '欢迎加入读书会！' })).toBeVisible()

  await ctxA.close()
  await ctxB.close()
})

test('the chat dock sends without flooding the server and loads older messages', async ({ page }) => {
  const requests: string[] = []
  page.on('request', (request) => {
    if (request.url().includes('/api/chat/')) requests.push(`${request.method()} ${new URL(request.url()).pathname}`)
  })
  await loginAsAdmin(page, 'zh')
  // More than one page (50) of the square, sent before the dock opens
  await page.evaluate(async () => {
    const me = (await (await fetch('/api/auth/me')).json()) as { csrfToken: string }
    for (let i = 1; i <= 60; i++)
      await fetch('/api/chat/channels/square/messages', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': me.csrfToken },
        body: JSON.stringify({ channelId: 'square', content: `earlier ${i}` }),
      })
  })
  requests.length = 0

  await page.getByRole('button', { name: '实时聊天' }).click()
  await page.getByText('小屋广场').click()
  const bubbles = page.locator('.rounded-2xl')
  await expect(bubbles.filter({ hasText: /^earlier 60$/ })).toBeVisible()
  await expect(bubbles.filter({ hasText: /^earlier 5$/ })).toHaveCount(0)
  await page.getByRole('button', { name: '加载更早消息' }).click()
  await expect(bubbles.filter({ hasText: /^earlier 5$/ })).toBeAttached()

  const input = page.getByPlaceholder('输入消息… (Enter 发送, Shift+Enter 换行)')
  await input.fill('一条新消息')
  await input.press('Enter')
  await expect(input).toHaveValue('')
  await expect(bubbles.filter({ hasText: '一条新消息' })).toHaveCount(1)
  await page.waitForTimeout(3000)
  // Each new message is marked read once, not on every render (that loop once took every connection)
  expect(requests.filter((r) => r.endsWith('/read')).length).toBeLessThanOrEqual(4)
  expect(requests.length).toBeLessThan(40)
  await expect(bubbles.filter({ hasText: /^earlier 5$/ })).toBeAttached()
})

test('a moderator removes chat messages: gone from every page showing them', async ({ page, browser }) => {
  await loginAsAdmin(page, 'zh')
  await page.evaluate(async () => {
    const me = (await (await fetch('/api/auth/me')).json()) as { csrfToken: string }
    await fetch('/api/admin/users', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': me.csrfToken },
      body: JSON.stringify({
        username: 'chatty',
        displayName: '话痨',
        email: 'chatty@example.com',
        password: 'password1234',
        emailVerified: true,
      }),
    })
  })

  const readerContext = await browser.newContext()
  const reader = await readerContext.newPage()
  await reader.goto('/zh/login')
  await reader.getByLabel('用户名或邮箱').fill('chatty')
  await reader.getByLabel('密码').fill('password1234')
  await reader.getByRole('button', { name: '登录' }).click()
  await expect(reader.getByRole('button', { name: '话痨' })).toBeVisible()
  await reader.getByRole('button', { name: '实时聊天' }).click()
  await reader.getByText('小屋广场').click()
  const input = reader.getByPlaceholder('输入消息… (Enter 发送, Shift+Enter 换行)')
  await input.fill('要被删掉的话')
  await input.press('Enter')
  await input.fill('也会被清空的话')
  await input.press('Enter')
  const readerBubbles = reader.locator('.rounded-2xl')
  await expect(readerBubbles.filter({ hasText: '也会被清空的话' })).toBeVisible()

  await page.getByRole('button', { name: '实时聊天' }).click()
  await page.getByText('小屋广场').click()
  const doomed = page.locator('.group').filter({ hasText: '要被删掉的话' })
  await doomed.hover()
  await doomed.getByRole('button', { name: '删除这条消息' }).click()
  await expect(page.getByText('消息已删除')).toBeVisible()
  await expect(page.locator('.rounded-2xl').filter({ hasText: '要被删掉的话' })).toHaveCount(0)
  await expect(readerBubbles.filter({ hasText: '要被删掉的话' })).toHaveCount(0)

  await page.getByRole('button', { name: '管理聊天' }).click()
  await page.getByRole('menuitem', { name: '清空消息' }).click()
  await page.getByRole('alertdialog').getByRole('button', { name: '清空消息' }).click()
  await expect(page.getByText(/已删除 \d+ 条消息/)).toBeVisible()
  await expect(reader.getByText('还没有消息，来发第一条吧！')).toBeVisible()
  // Readers have no such menu
  await expect(reader.getByRole('button', { name: '管理聊天' })).toHaveCount(0)
  await readerContext.close()
})
