import { useLocation, useParams } from 'react-router'
import { isLang, type Lang } from './schemas'

// The page's language, from /:lang. lang.tsx renders the 404 page for anything else, so below it this
// always holds
export function useLang(): Lang {
  const { lang } = useParams()
  return isLang(lang) ? lang : 'zh'
}

export function localePath(lang: Lang, path = ''): string {
  return `/${lang}${path}`
}

// The same page in another language: the first segment swapped
export function switchLang(pathname: string, lang: Lang): string {
  const rest = pathname.replace(/^\/(zh|en)(?=\/|$)/, '')
  return `/${lang}${rest}`
}

// The reader's preferred language of ours, for "/"
export function preferredLang(languages: readonly string[]): Lang {
  for (const language of languages) {
    const primary = language.toLowerCase().split('-')[0]
    if (primary === 'zh' || primary === 'en') return primary
  }
  return 'zh'
}

// Where posts are edited from here: the admin's pages, or a writer's (/write)
export function usePostsBase(): string {
  const { pathname } = useLocation()
  return /^\/(zh|en)\/admin(\/|$)/.test(pathname) ? '/admin/posts' : '/write'
}
