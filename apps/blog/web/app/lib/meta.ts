import type { MetaDescriptor } from 'react-router'
import { langs, type Lang } from './schemas'

export const publicOrigin = __PUBLIC_ORIGIN__

interface PageMeta {
  lang: Lang
  title: string
  description?: string
  // Without the language: "/posts/x"
  path: string
  // The languages the page exists in, for hreflang; all by default
  languages?: readonly Lang[]
  image?: string
  type?: 'website' | 'article'
}

export function pageMeta({ lang, title, description, path, languages = langs, image, type = 'website' }: PageMeta): MetaDescriptor[] {
  const url = `${publicOrigin}/${lang}${path}`
  const meta: MetaDescriptor[] = [
    { title },
    { tagName: 'link', rel: 'canonical', href: url },
    { property: 'og:title', content: title },
    { property: 'og:url', content: url },
    { property: 'og:type', content: type },
    { property: 'og:locale', content: lang === 'zh' ? 'zh_CN' : 'en_US' },
  ]

  if (description) {
    meta.push({ name: 'description', content: description })
    meta.push({ property: 'og:description', content: description })
  }

  if (image) meta.push({ property: 'og:image', content: image.startsWith('/') ? `${publicOrigin}${image}` : image })

  for (const other of languages) meta.push({ tagName: 'link', rel: 'alternate', hrefLang: other, href: `${publicOrigin}/${other}${path}` })

  return meta
}
