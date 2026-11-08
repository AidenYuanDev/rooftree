import { readFileSync } from 'node:fs'
import { expect, type Cookie, type Page } from '@playwright/test'
import { adminPassword, lastMailTo, totp } from '../../tests/server.mjs'
import { stateFile } from './global-setup'

interface State {
  adminTotpSecret: string
  outbox: string
}

export const state = (): State => JSON.parse(readFileSync(stateFile, 'utf8')) as State

export function mailTo(address: string) {
  return lastMailTo(state().outbox, address) as { text: string; token: string } | undefined
}

// TOTP codes can't be reused and the server takes one step around now at most: each login uses a
// later step than the one before, waiting for time to move on when it has to
let lastStep = 0

async function nextTotp(): Promise<string> {
  for (;;) {
    const now = Math.floor(Date.now() / 30000)
    const step = Math.max(now, lastStep + 1)
    if (step <= now + 1) {
      lastStep = step
      return totp(state().adminTotpSecret, step - now)
    }
    await new Promise((resolve) => setTimeout(resolve, (now + 1) * 30000 - Date.now() + 100))
  }
}

// The admin's cookies after its first login, for the next tests. A login of its own each would need a
// later TOTP step every time: after two logins in one 30-second window, the third waits for the next
// window, up to 30 seconds, the whole of a test's timeout (a test failing now and then, depending on
// where in the window the run got to it)
let adminCookies: Cookie[] | undefined

export async function loginAsAdmin(page: Page, lang: 'zh' | 'en' = 'en') {
  if (adminCookies) {
    await page.context().addCookies(adminCookies)
    await page.goto(`/${lang}`)
    await expect(page.getByRole('button', { name: 'The Author' })).toBeVisible()
    return
  }

  await page.goto(`/${lang}/login`)
  await page.getByLabel(lang === 'en' ? 'User name or email' : '用户名或邮箱').fill('author')
  await page.getByLabel(lang === 'en' ? 'Password' : '密码', { exact: true }).fill(adminPassword)
  await page.getByRole('button', { name: lang === 'en' ? 'Log in' : '登录' }).click()
  await page.getByLabel(lang === 'en' ? 'Verification code' : '两步验证码').fill(await nextTotp())
  await page.getByRole('button', { name: lang === 'en' ? 'Log in' : '登录' }).click()
  await expect(page.getByRole('button', { name: 'The Author' })).toBeVisible()
  adminCookies = await page.context().cookies()
}
