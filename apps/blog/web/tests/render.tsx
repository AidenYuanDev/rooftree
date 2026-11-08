import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { render } from '@testing-library/react'
import { I18nextProvider } from 'react-i18next'
import { createRoutesStub } from 'react-router'
import { getI18n } from '~/i18n'
import type { Lang } from '~/lib/schemas'

interface StubRoute {
  path: string
  Component: React.ComponentType
}

// A page at a URL like the app renders it: under /:lang with its i18n, a fresh query client, and
// any other routes it navigates to
export function renderPage(url: string, routes: StubRoute[]) {
  const lang = (url.split('/')[1] ?? 'zh') as Lang
  const queryClient = new QueryClient({ defaultOptions: { queries: { retry: false }, mutations: { retry: false } } })
  const Stub = createRoutesStub(
    routes.map((route) => ({
      path: route.path,
      Component: () => (
        <I18nextProvider i18n={getI18n(lang)}>
          <route.Component />
        </I18nextProvider>
      ),
    })),
  )

  return {
    queryClient,
    ...render(
      <QueryClientProvider client={queryClient}>
        <Stub initialEntries={[url]} />
      </QueryClientProvider>,
    ),
  }
}
