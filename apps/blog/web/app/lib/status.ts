import type { ServerStatus } from './schemas'

// The status page asks for the day once, then every few seconds only for the samples after the last
// one it has (?since=): those are added to it, and what is more than a day older than the newest goes
export const statusWindow = 24 * 60 * 60

export function mergeStatus(before: ServerStatus | undefined, next: ServerStatus): ServerStatus {
  if (!before) return next
  // Restarted meanwhile: the server's day starts over, so does the page's
  if (next.now.uptime < before.now.uptime) return next

  const keys = Object.keys(next.series) as (keyof ServerStatus['series'])[]
  const at = [...before.series.at, ...next.series.at]
  const newest = at.at(-1) ?? 0
  const first = at.findIndex((time) => time > newest - statusWindow)
  const series = Object.fromEntries(
    keys.map((key) => [key, [...before.series[key], ...next.series[key]].slice(first)]),
  ) as ServerStatus['series']
  return { ...next, series }
}

// "3 天 4 小时", "2 h 5 min", "1 d": the biggest unit, and the one after it unless that is 0
export function formatUptime(seconds: number, lang: 'zh' | 'en'): string {
  const units: [number, string, string][] = [
    [86400, ' 天', ' d'],
    [3600, ' 小时', ' h'],
    [60, ' 分钟', ' min'],
    [1, ' 秒', ' s'],
  ]
  const total = Math.max(0, Math.floor(seconds))
  const found = units.findIndex(([size]) => total >= size)
  // 0: in seconds
  const index = found === -1 ? units.length - 1 : found
  const name = (unit: [number, string, string]) => (lang === 'zh' ? unit[1] : unit[2])
  const unit = units[index]!
  const parts = [`${Math.floor(total / unit[0])}${name(unit)}`]
  const next = units[index + 1]
  const rest = next ? Math.floor((total % unit[0]) / next[0]) : 0
  if (next && rest) parts.push(`${rest}${name(next)}`)
  return parts.join(' ')
}

// A sample's time of day, in the reader's zone (the page renders in the browser only)
export function formatTime(at: number, lang: string) {
  return new Intl.DateTimeFormat(lang === 'zh' ? 'zh-CN' : 'en-US', { hour: '2-digit', minute: '2-digit', hour12: false }).format(
    new Date(at * 1000),
  )
}

// The runs of samples without a gap: a restart, or a hang, breaks the line instead of bridging it
export function runs(at: number[], interval: number): [number, number][] {
  const result: [number, number][] = []
  let start = 0
  for (let i = 1; i <= at.length; i++) {
    if (i === at.length || at[i]! - at[i - 1]! > interval * 3) {
      if (i > start) result.push([start, i])
      start = i
    }
  }
  return result
}
