import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import { CottageMark } from '~/components/scenery/mark'
import { Button } from '~/components/ui/button'
import { localePath, useLang } from '~/lib/lang'

export function NotFoundPage() {
  const { t } = useTranslation()
  const lang = useLang()
  return (
    <div className="flex flex-col items-center gap-4 py-16 text-center">
      <CottageMark className="size-24" />
      <p className="font-heading text-6xl font-bold text-muted-foreground">404</p>
      <h1 className="text-xl font-semibold">{t('common.notFound')}</h1>
      <p className="text-muted-foreground">{t('common.notFoundHill')}</p>
      <Button asChild variant="outline">
        <Link to={localePath(lang)}>{t('common.backHome')}</Link>
      </Button>
    </div>
  )
}

// Unknown addresses are served by the SPA fallback with a 200 (the server can't know): keep them out of indexes
export const meta = () => [{ title: '404' }, { name: 'robots', content: 'noindex' }]

export default NotFoundPage
