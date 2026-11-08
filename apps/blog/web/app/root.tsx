import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { useEffect, useState } from 'react'
import { isRouteErrorResponse, Links, Meta, Outlet, Scripts, ScrollRestoration, useParams } from 'react-router'
import type { Route } from './+types/root'
import { CottageMark } from '~/components/scenery/mark'
import { Toaster } from '~/components/ui/sonner'
import { getI18n } from '~/i18n'
import { isLang } from '~/lib/schemas'
import { applyTheme, themeScript, useThemeStore } from '~/stores/theme'
import './app.css'

export function Layout({ children }: { children: React.ReactNode }) {
  const { lang } = useParams()
  return (
    <html lang={lang === 'en' ? 'en' : 'zh-CN'} suppressHydrationWarning>
      <head>
        <meta charSet="utf-8" />
        <meta name="viewport" content="width=device-width, initial-scale=1" />
        <link rel="icon" href="/favicon.svg" type="image/svg+xml" />
        <script dangerouslySetInnerHTML={{ __html: themeScript }} />
        <link rel="alternate" type="application/rss+xml" href={`/${lang === 'en' ? 'en' : 'zh'}/feed.xml`} />
        <Meta />
        <Links />
      </head>
      <body className="min-h-dvh bg-background text-foreground antialiased">
        {children}
        <ScrollRestoration />
        <Scripts />
      </body>
    </html>
  )
}

function ThemeSync() {
  const theme = useThemeStore((state) => state.theme)
  useEffect(() => {
    applyTheme(theme)
    if (theme !== 'system') return
    const query = window.matchMedia('(prefers-color-scheme: dark)')
    const listener = () => applyTheme('system')
    query.addEventListener('change', listener)
    return () => query.removeEventListener('change', listener)
  }, [theme])
  return null
}

export default function App() {
  // One client per app: the prerender build renders pages one after another, each gets its own
  const [queryClient] = useState(() => new QueryClient({ defaultOptions: { queries: { retry: 1, refetchOnWindowFocus: false } } }))
  return (
    <QueryClientProvider client={queryClient}>
      <ThemeSync />
      <Outlet />
      <Toaster position="top-center" />
    </QueryClientProvider>
  )
}

// While a page that wasn't prerendered (SPA fallback) loads its data
export function HydrateFallback() {
  return (
    <div className="flex min-h-dvh items-center justify-center">
      <CottageMark className="size-12" />
    </div>
  )
}

export function ErrorBoundary({ error, params }: Route.ErrorBoundaryProps) {
  const i18n = getI18n(isLang(params.lang) ? params.lang : 'zh')
  const notFound = isRouteErrorResponse(error) && error.status === 404
  return (
    <main className="mx-auto flex min-h-dvh max-w-xl flex-col items-center justify-center gap-4 p-6 text-center">
      <h1 className="text-2xl font-semibold">{notFound ? i18n.t('common.notFound') : i18n.t('common.error')}</h1>
      {!notFound && import.meta.env.DEV && error instanceof Error && (
        <pre className="max-w-full overflow-auto text-left text-xs">{error.stack}</pre>
      )}
      <a className="text-primary underline" href={`/${isLang(params.lang) ? params.lang : 'zh'}`}>
        {i18n.t('common.backHome')}
      </a>
    </main>
  )
}
