import { preferredLang, switchLang } from './lang'

describe('switchLang', () => {
  test('swaps the first segment only', () => {
    expect(switchLang('/zh/posts/zh-and-en', 'en')).toBe('/en/posts/zh-and-en')
    expect(switchLang('/en', 'zh')).toBe('/zh')
    expect(switchLang('/zh/', 'en')).toBe('/en/')
  })
})

describe('preferredLang', () => {
  test('takes the first of ours', () => {
    expect(preferredLang(['fr-FR', 'en-US', 'zh-CN'])).toBe('en')
    expect(preferredLang(['zh-TW'])).toBe('zh')
    expect(preferredLang(['de'])).toBe('zh')
  })
})
