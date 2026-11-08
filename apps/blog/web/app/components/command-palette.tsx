import { FileTextIcon, LanguagesIcon, MoonIcon, SearchIcon, SunIcon } from 'lucide-react'
import { useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { useLocation, useNavigate } from 'react-router'
import { Command, CommandEmpty, CommandGroup, CommandInput, CommandItem, CommandList } from '~/components/ui/command'
import { Dialog, DialogContent, DialogDescription, DialogTitle } from '~/components/ui/dialog'
import { localePath, switchLang, useLang } from '~/lib/lang'
import { mainNav, moreNav } from '~/lib/nav'
import { useSearch } from '~/lib/queries'
import { isSearchable } from '~/lib/text'
import { useDebounced } from '~/lib/use-debounced'
import { usePaletteStore } from '~/stores/palette'
import { useThemeStore } from '~/stores/theme'

// ⌘K / Ctrl+K anywhere, or the header's search button: the site's pages, the posts matching what's
// typed (the search API, in the page's language; other languages on the search page) and a few actions

export function CommandPalette() {
  const { t } = useTranslation()
  const lang = useLang()
  const navigate = useNavigate()
  const location = useLocation()
  const { open, setOpen } = usePaletteStore()
  const setTheme = useThemeStore((state) => state.setTheme)
  const [query, setQuery] = useState('')
  const search = useSearch(lang, useDebounced(query))
  const long = isSearchable(query)

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.key.toLowerCase() === 'k' && (event.metaKey || event.ctrlKey)) {
        event.preventDefault()
        setOpen(!usePaletteStore.getState().open)
      }
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [setOpen])

  const go = (path: string) => {
    setOpen(false)
    setQuery('')
    void navigate(path)
  }

  const typed = query.trim().toLowerCase()
  const pages = [...mainNav, ...moreNav].filter((item) => !typed || t(`nav.${item.key}`).toLowerCase().includes(typed))
  const posts = long ? (search.data?.posts ?? []) : []

  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogContent className="overflow-hidden p-0 sm:max-w-xl" showCloseButton={false}>
        <DialogTitle className="sr-only">{t('search.title')}</DialogTitle>
        <DialogDescription className="sr-only">{t('search.paletteHint')}</DialogDescription>
        <Command shouldFilter={false}>
          <CommandInput value={query} onValueChange={setQuery} placeholder={t('search.paletteHint')} />
          <CommandList>
            <CommandEmpty>{long ? t('search.empty', { query }) : t('search.hint')}</CommandEmpty>
            {posts.length > 0 && (
              <CommandGroup heading={t('nav.posts')}>
                {posts.map((post) => (
                  <CommandItem key={post.id} value={`post-${post.id}`} onSelect={() => go(localePath(lang, `/posts/${post.slug}`))}>
                    <FileTextIcon />
                    <span className="truncate" lang={post.lang}>
                      {post.title}
                    </span>
                  </CommandItem>
                ))}
                <CommandItem value="search-all" onSelect={() => go(`${localePath(lang, '/search')}?q=${encodeURIComponent(query)}`)}>
                  <SearchIcon />
                  {t('search.all', { query })}
                </CommandItem>
              </CommandGroup>
            )}
            {long && search.isSuccess && (
              <CommandGroup heading={posts.length ? undefined : t('search.empty', { query })}>
                <CommandItem
                  value="search-all-langs"
                  onSelect={() => go(`${localePath(lang, '/search')}?q=${encodeURIComponent(query)}&all=1`)}
                >
                  <LanguagesIcon />
                  {t('search.inAllLangs', { query })}
                </CommandItem>
              </CommandGroup>
            )}
            {pages.length > 0 && (
              <CommandGroup heading={t('search.pages')}>
                {pages.map((item) => (
                  <CommandItem key={item.key} value={`page-${item.key}`} onSelect={() => go(localePath(lang, item.path))}>
                    {t(`nav.${item.key}`)}
                  </CommandItem>
                ))}
              </CommandGroup>
            )}
            {!typed && (
              <CommandGroup heading={t('search.actions')}>
                <CommandItem value="theme-light" onSelect={() => (setTheme('light'), setOpen(false))}>
                  <SunIcon />
                  {t('nav.themeLight')}
                </CommandItem>
                <CommandItem value="theme-dark" onSelect={() => (setTheme('dark'), setOpen(false))}>
                  <MoonIcon />
                  {t('nav.themeDark')}
                </CommandItem>
                <CommandItem value="lang" onSelect={() => go(switchLang(location.pathname, lang === 'zh' ? 'en' : 'zh') + location.search)}>
                  <LanguagesIcon />
                  {t(`langs.${lang === 'zh' ? 'en' : 'zh'}`)}
                </CommandItem>
              </CommandGroup>
            )}
          </CommandList>
        </Command>
      </DialogContent>
    </Dialog>
  )
}
