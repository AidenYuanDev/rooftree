import type { MetaDescriptor } from 'react-router'
import { getI18n } from '~/i18n'
import type { Resources } from '~/i18n/zh'

type Key = { [S in keyof Resources]: `${S & string}.${keyof Resources[S] & string}` }[keyof Resources]

// Account and admin pages: a title, and kept out of search engines
export function privateMeta(lang: string | undefined, titleKey: Key): MetaDescriptor[] {
  const i18n = getI18n(lang === 'en' ? 'en' : 'zh')
  return [{ title: `${i18n.t(titleKey as 'site.name')} · ${i18n.t('site.name')}` }, { name: 'robots', content: 'noindex' }]
}
