// One i18next instance per language, created up front with the resources inlined: the prerender build
// renders zh and en pages one after another, and no page may see another one's language. The
// language layout (routes/lang.tsx) picks the instance from the URL and provides it.

import i18next, { type i18n } from 'i18next'
import { initReactI18next } from 'react-i18next'
import { ApiError } from '~/lib/api'
import type { Lang } from '~/lib/schemas'
import { en } from './en'
import { zh } from './zh'

export const resources = { zh: { translation: zh }, en: { translation: en } }

declare module 'i18next' {
  interface CustomTypeOptions {
    resources: { translation: typeof zh }
  }
}

function create(lang: Lang): i18n {
  const instance = i18next.createInstance()
  instance.use(initReactI18next)
  // initAsync false: the resources are here, init finishes before this returns
  void instance.init({ lng: lang, fallbackLng: 'zh', resources, initAsync: false, interpolation: { escapeValue: false } })
  return instance
}

const instances: Record<Lang, i18n> = { zh: create('zh'), en: create('en') }

export function getI18n(lang: Lang): i18n {
  return instances[lang]
}

// "network_error" -> the translated message of an API error code. A request that was too soon says
// how long to wait when there is a "<code>_wait" message
export function errorMessage(i18n: i18n, error: unknown): string {
  const code = error instanceof ApiError ? error.code : 'unknown'
  const key = `errors.${code}`
  if (error instanceof ApiError && error.retryAfter && i18n.exists(`${key}_wait`))
    return i18n.t(`${key}_wait` as 'errors.too_many_mails_wait', { seconds: error.retryAfter })
  return i18n.exists(key) ? i18n.t(key as 'errors.unknown') : i18n.t('errors.unknown', { code })
}

export function fieldErrorMessage(i18n: i18n, code: string): string {
  const key = `fieldErrors.${code}`
  return i18n.exists(key) ? i18n.t(key as 'fieldErrors.invalid') : code
}

export function formatDate(lang: Lang, iso: string | null | undefined): string {
  if (!iso) return ''
  return new Intl.DateTimeFormat(lang === 'zh' ? 'zh-CN' : 'en-US', { year: 'numeric', month: 'long', day: 'numeric' }).format(
    new Date(iso),
  )
}

// 3725 -> "1 小时 2 分钟", the two largest units; under a minute "不到 1 分钟"
export function formatDuration(i18n: i18n, seconds: number): string {
  const minutes = Math.floor(seconds / 60)
  if (minutes < 1) return i18n.t('duration.lessThanMinute')
  const parts = [
    ['days', Math.floor(minutes / 1440)],
    ['hours', Math.floor(minutes / 60) % 24],
    ['minutes', minutes % 60],
  ] as const
  const first = parts.findIndex(([, count]) => count > 0)
  return parts
    .slice(first, first + 2)
    .filter(([, count]) => count > 0)
    .map(([unit, count]) => i18n.t(`duration.${unit}`, { count }))
    .join(' ')
}

// "3 天前", "yesterday", from now
export function formatRelative(lang: Lang, iso: string, now: number): string {
  const seconds = (Date.parse(iso) - now) / 1000
  const format = new Intl.RelativeTimeFormat(lang === 'zh' ? 'zh-CN' : 'en-US', { numeric: 'auto' })
  for (const [unit, size] of [
    ['year', 31_536_000],
    ['month', 2_592_000],
    ['day', 86_400],
    ['hour', 3600],
    ['minute', 60],
  ] as const) {
    if (Math.abs(seconds) >= size) return format.format(Math.round(seconds / size), unit)
  }
  return format.format(0, 'second')
}
