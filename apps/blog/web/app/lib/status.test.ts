import type { ServerStatus } from './schemas'
import { formatUptime, mergeStatus, runs, statusWindow } from './status'

function status(uptime: number, at: number[]): ServerStatus {
  const column = at.map(() => 1)
  const now = {
    readers: 1,
    pages: 1,
    requests: 1,
    updateAvgUs: 1,
    updateMaxUs: 1,
    memoryMiB: 1,
    cpuPercent: 1,
    load: 1,
    hostMemoryPercent: 1,
    diskFreePercent: 1,
  }
  const series = Object.fromEntries(Object.keys(now).map((key) => [key, column])) as Omit<ServerStatus['series'], 'at'>
  return {
    now: { ...now, at: at.at(-1) ?? 0, uptime, cores: 2, chatMessagesTotal: 0, chatChannelsCount: 0 },
    interval: 60,
    series: { ...series, at },
  }
}

test('new samples are added, a day is kept', () => {
  const merged = mergeStatus(status(100, [60, 120]), status(200, [180]))
  expect(merged.series.at).toEqual([60, 120, 180])
  expect(merged.series.readers).toHaveLength(3)
  expect(merged.now.uptime).toBe(200)

  const later = mergeStatus(merged, status(300, [180 + statusWindow]))
  expect(later.series.at).toEqual([180 + statusWindow])
  expect(later.series.load).toEqual([1])
})

test('after a restart the page starts over', () => {
  expect(mergeStatus(status(5000, [60, 120]), status(10, [180])).series.at).toEqual([180])
  expect(mergeStatus(undefined, status(10, [180])).series.at).toEqual([180])
})

test('uptime in its two biggest units', () => {
  expect(formatUptime(0, 'en')).toBe('0 s')
  expect(formatUptime(59, 'zh')).toBe('59 秒')
  expect(formatUptime(3 * 86400 + 4 * 3600 + 5 * 60, 'zh')).toBe('3 天 4 小时')
  expect(formatUptime(2 * 3600 + 5 * 60 + 9, 'en')).toBe('2 h 5 min')
  expect(formatUptime(86400 + 30, 'en')).toBe('1 d')
})

test('a gap breaks the line', () => {
  expect(runs([], 60)).toEqual([])
  expect(runs([60, 120, 180, 900, 960], 60)).toEqual([
    [0, 3],
    [3, 5],
  ])
})
