import type { Resources } from '~/i18n/zh'

// The site's sections, for the header, the mobile menu, the footer and the command palette.
// key: nav.<key> in i18n; path: without the language

export interface NavItem {
  key: keyof Resources['nav']
  path: string
}

export const mainNav: NavItem[] = [
  { key: 'home', path: '' },
  { key: 'posts', path: '/posts' },
  { key: 'notes', path: '/notes' },
  { key: 'timeline', path: '/timeline' },
  { key: 'thinking', path: '/thinking' },
]

// Under "More"
export const moreNav: NavItem[] = [
  { key: 'friends', path: '/friends' },
  { key: 'projects', path: '/projects' },
  { key: 'guestbook', path: '/guestbook' },
  { key: 'about', path: '/about' },
]

// Which item a page belongs to: the longest path its address starts with
export function activeNav(pathname: string, items: NavItem[]): NavItem | undefined {
  const rest = pathname.replace(/^\/(zh|en)(?=\/|$)/, '')
  return items
    .filter((item) => (item.path === '' ? rest === '' || rest === '/' : rest === item.path || rest.startsWith(`${item.path}/`)))
    .sort((a, b) => b.path.length - a.path.length)[0]
}
