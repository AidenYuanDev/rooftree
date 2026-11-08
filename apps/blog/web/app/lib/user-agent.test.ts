import { describe, expect, test } from 'vitest'
import { describeUserAgent } from './user-agent'

describe('describeUserAgent', () => {
  test('browser and system', () => {
    expect(
      describeUserAgent(
        'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36 Edg/130.0',
      ),
    ).toBe('Edge · Windows')
    expect(
      describeUserAgent('Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 Version/18.0 Mobile Safari/604.1'),
    ).toBe('Safari · iOS')
    expect(describeUserAgent('Mozilla/5.0 (X11; Linux x86_64; rv:131.0) Gecko/20100101 Firefox/131.0')).toBe('Firefox · Linux')
  })

  test('unknown ones as they are', () => {
    expect(describeUserAgent('curl/8.10')).toBe('curl/8.10')
    expect(describeUserAgent('')).toBe('?')
  })
})
