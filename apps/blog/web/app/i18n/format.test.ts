import { describe, expect, test } from 'vitest'
import { formatDuration, formatRelative, getI18n } from '~/i18n'

describe('formatDuration', () => {
  test('the two largest units', () => {
    const zh = getI18n('zh')
    expect(formatDuration(zh, 30)).toBe('不到 1 分钟')
    expect(formatDuration(zh, 3725)).toBe('1 小时 2 分钟')
    expect(formatDuration(zh, 2 * 86_400 + 3 * 3600 + 59)).toBe('2 天 3 小时')
    expect(formatDuration(getI18n('en'), 86_400 + 120)).toBe('1 day')
    expect(formatDuration(getI18n('en'), 7260)).toBe('2 hours 1 minute')
  })
})

describe('formatRelative', () => {
  test('from now', () => {
    const now = Date.parse('2026-10-27T12:00:00Z')
    expect(formatRelative('en', '2026-10-27T11:57:00Z', now)).toBe('3 minutes ago')
    expect(formatRelative('zh', '2026-10-24T12:00:00Z', now)).toBe('3天前')
    expect(formatRelative('en', '2026-10-27T12:00:00Z', now)).toBe('now')
  })
})
