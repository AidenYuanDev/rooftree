import { ChevronDownIcon, MenuIcon, SearchIcon, XIcon } from 'lucide-react'
import { AnimatePresence, motion, useMotionValueEvent, useScroll } from 'motion/react'
import { Dialog as DialogPrimitive } from 'radix-ui'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Link, NavLink, useLocation } from 'react-router'
import { CottageMark } from '~/components/scenery/mark'
import { LangSwitcher } from '~/components/lang-switcher'
import { ThemeToggle } from '~/components/theme-toggle'
import { Button } from '~/components/ui/button'
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from '~/components/ui/dropdown-menu'
import { UserMenu } from '~/components/user-menu'
import { localePath, useLang } from '~/lib/lang'
import { activeNav, mainNav, moreNav } from '~/lib/nav'
import { useSiteName } from '~/lib/site-name'
import { cn } from '~/lib/utils'
import { useHeaderStore } from '~/stores/header'
import { usePaletteStore } from '~/stores/palette'

// At the top of a page the header is just there, over the painting; scrolled, it becomes a glass bar.
// Reading down a post, the middle shows the post's title instead of the sections; scrolling back
// up brings the sections back.

function useScrollState() {
  const { scrollY } = useScroll()
  const [state, setState] = useState({ scrolled: false, down: false })
  useMotionValueEvent(scrollY, 'change', (y) => {
    const previous = scrollY.getPrevious() ?? 0
    const scrolled = y > 24
    // Some hysteresis: a few pixels back up don't count
    const down = y > 280 && (y > previous + 2 ? true : y < previous - 6 ? false : state.down)
    if (scrolled !== state.scrolled || down !== state.down) setState({ scrolled, down })
  })
  return state
}

function DesktopNav() {
  const { t } = useTranslation()
  const lang = useLang()
  const { pathname } = useLocation()
  const active = activeNav(pathname, [...mainNav, ...moreNav])
  const moreActive = active && moreNav.includes(active)

  return (
    <nav aria-label={t('nav.main')} className="flex items-center rounded-full border bg-card px-1 py-1 shadow-sm backdrop-blur-md">
      {mainNav.map((item) => (
        <NavLink
          key={item.key}
          to={localePath(lang, item.path)}
          end={item.path === ''}
          className={cn(
            'relative rounded-full px-3.5 py-1.5 text-sm transition-colors',
            active === item ? 'text-primary-foreground' : 'text-muted-foreground hover:text-foreground',
          )}
        >
          {active === item && (
            <motion.span
              layoutId="nav-active"
              className="absolute inset-0 -z-10 rounded-full bg-primary"
              transition={{ type: 'spring', stiffness: 420, damping: 34 }}
            />
          )}
          {t(`nav.${item.key}`)}
        </NavLink>
      ))}
      <DropdownMenu>
        <DropdownMenuTrigger
          className={cn(
            'relative flex items-center gap-1 rounded-full px-3.5 py-1.5 text-sm outline-none',
            moreActive ? 'text-primary-foreground' : 'text-muted-foreground hover:text-foreground',
          )}
        >
          {moreActive && (
            <motion.span
              layoutId="nav-active"
              className="absolute inset-0 -z-10 rounded-full bg-primary"
              transition={{ type: 'spring', stiffness: 420, damping: 34 }}
            />
          )}
          {t('nav.more')}
          <ChevronDownIcon className="size-3.5" />
        </DropdownMenuTrigger>
        <DropdownMenuContent align="center">
          {moreNav.map((item) => (
            <DropdownMenuItem key={item.key} asChild>
              <Link to={localePath(lang, item.path)}>{t(`nav.${item.key}`)}</Link>
            </DropdownMenuItem>
          ))}
        </DropdownMenuContent>
      </DropdownMenu>
    </nav>
  )
}

function MobileNav() {
  const { t } = useTranslation()
  const lang = useLang()
  const { pathname } = useLocation()
  const [open, setOpen] = useState(false)
  const active = activeNav(pathname, [...mainNav, ...moreNav])
  return (
    <DialogPrimitive.Root open={open} onOpenChange={setOpen}>
      <DialogPrimitive.Trigger asChild>
        <Button variant="ghost" size="icon" className="lg:hidden" aria-label={t('nav.menu')}>
          <MenuIcon />
        </Button>
      </DialogPrimitive.Trigger>
      <DialogPrimitive.Portal>
        <DialogPrimitive.Overlay className="fixed inset-0 z-50 bg-black/40 backdrop-blur-sm data-[state=closed]:animate-out data-[state=closed]:fade-out-0 data-[state=open]:animate-in data-[state=open]:fade-in-0" />
        <DialogPrimitive.Content className="fixed inset-y-0 right-0 z-50 flex w-72 max-w-[85vw] flex-col gap-1 border-l bg-popover p-4 shadow-xl data-[state=closed]:animate-out data-[state=closed]:slide-out-to-right data-[state=open]:animate-in data-[state=open]:slide-in-from-right">
          <div className="mb-4 flex items-center justify-between">
            <DialogPrimitive.Title className="font-heading text-lg">{t('nav.menu')}</DialogPrimitive.Title>
            <DialogPrimitive.Close asChild>
              <Button variant="ghost" size="icon" aria-label={t('common.close')}>
                <XIcon />
              </Button>
            </DialogPrimitive.Close>
          </div>
          <DialogPrimitive.Description className="sr-only">{t('nav.main')}</DialogPrimitive.Description>
          {[...mainNav, ...moreNav].map((item) => (
            <Link
              key={item.key}
              to={localePath(lang, item.path)}
              onClick={() => setOpen(false)}
              className={cn(
                'rounded-lg px-3 py-2.5 transition-colors',
                active === item ? 'bg-primary text-primary-foreground' : 'hover:bg-accent',
              )}
            >
              {t(`nav.${item.key}`)}
            </Link>
          ))}
        </DialogPrimitive.Content>
      </DialogPrimitive.Portal>
    </DialogPrimitive.Root>
  )
}

export function SiteHeader() {
  const { t } = useTranslation()
  const lang = useLang()
  const siteName = useSiteName()
  const title = useHeaderStore((state) => state.title)
  const openPalette = usePaletteStore((state) => state.setOpen)
  const { scrolled, down } = useScrollState()
  const showTitle = Boolean(title) && down

  return (
    <header
      className={cn(
        'fixed inset-x-0 top-0 z-40 transition-[background-color,border-color,box-shadow] duration-300',
        scrolled ? 'border-b bg-background/70 shadow-sm backdrop-blur-lg' : 'border-b border-transparent',
      )}
    >
      <a
        href="#content"
        className="sr-only focus:not-sr-only focus:absolute focus:top-2 focus:left-2 focus:rounded focus:bg-background focus:p-2"
      >
        {t('site.skipToContent')}
      </a>
      <div className="mx-auto grid h-16 max-w-6xl grid-cols-[1fr_auto_1fr] items-center gap-2 px-4">
        <Link to={localePath(lang)} className="flex items-center gap-2 justify-self-start" aria-label={t('nav.home')}>
          <CottageMark className="size-8" />
          <span className="hidden font-heading text-lg font-bold tracking-wide whitespace-nowrap sm:inline">{siteName}</span>
        </Link>

        <div className="relative hidden h-10 items-center justify-center lg:flex">
          <AnimatePresence mode="wait" initial={false}>
            {showTitle ? (
              <motion.p
                key="title"
                initial={{ opacity: 0, y: 12 }}
                animate={{ opacity: 1, y: 0 }}
                exit={{ opacity: 0, y: -12 }}
                transition={{ type: 'spring', stiffness: 380, damping: 32 }}
                className="max-w-md truncate font-heading text-base font-bold"
              >
                {title}
              </motion.p>
            ) : (
              <motion.div
                key="nav"
                initial={{ opacity: 0, y: -12 }}
                animate={{ opacity: 1, y: 0 }}
                exit={{ opacity: 0, y: 12 }}
                transition={{ type: 'spring', stiffness: 380, damping: 32 }}
              >
                <DesktopNav />
              </motion.div>
            )}
          </AnimatePresence>
        </div>

        {/* In the third column even while the nav between is hidden */}
        <div className="col-start-3 flex items-center gap-0.5 justify-self-end">
          <Button variant="ghost" size="icon" onClick={() => openPalette(true)} aria-label={t('search.title')} title="⌘K">
            <SearchIcon />
          </Button>
          <LangSwitcher />
          <ThemeToggle />
          <UserMenu />
          <MobileNav />
        </div>
      </div>
    </header>
  )
}
