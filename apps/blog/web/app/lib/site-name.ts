import { useQuery } from '@tanstack/react-query'
import { useTranslation } from 'react-i18next'
import { useMatches } from 'react-router'
import { fetchSiteCached } from './content'
import { useLang } from './lang'

// The site's name for the header and footer: from the page's loader data (siteName, or the home page's
// site), so prerendered pages have it in their HTML; pages without loaders (admin, account) ask the API
export function useSiteName(): string {
  const { t } = useTranslation()
  const lang = useLang()
  const matches = useMatches()
  let fromPage: string | undefined
  for (const match of matches) {
    const data = match.loaderData as { siteName?: unknown; site?: { name?: unknown } } | undefined
    if (typeof data?.siteName === 'string') fromPage = data.siteName
    else if (typeof data?.site?.name === 'string') fromPage = data.site.name
  }

  const site = useQuery({
    queryKey: ['site', lang],
    queryFn: () => fetchSiteCached(lang),
    enabled: fromPage === undefined,
    staleTime: 300_000,
  })
  return fromPage ?? site.data?.name ?? t('site.name')
}
