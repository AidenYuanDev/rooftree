import { useTranslation } from 'react-i18next'
import { Link, useSearchParams } from 'react-router'
import type { Route } from './+types/unsubscribe'
import { CottageMark } from '~/components/scenery/mark'
import { Button } from '~/components/ui/button'
import { useHydrated } from '~/lib/hydrated'
import { localePath, useLang } from '~/lib/lang'
import { privateMeta } from '~/lib/page'
import { useUnsubscribe } from '~/lib/queries'

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'subscribe.unsubscribeTitle')

// The link in every newsletter. A button, not on load: mail scanners open links in mails
export default function Unsubscribe() {
  const { t } = useTranslation()
  const hydrated = useHydrated()
  const lang = useLang()
  const [searchParams] = useSearchParams()
  const token = searchParams.get('token') ?? ''
  const unsubscribe = useUnsubscribe()

  return (
    <div className="mx-auto flex max-w-md flex-col items-center gap-4 py-16 text-center">
      <CottageMark className="size-20" />
      <h1 className="font-heading text-2xl font-bold">{t('subscribe.unsubscribeTitle')}</h1>
      {unsubscribe.isSuccess ? (
        <p className="text-muted-foreground">{t('subscribe.unsubscribed')}</p>
      ) : (
        <>
          <p className="text-muted-foreground">{unsubscribe.isError || !token ? t('subscribe.invalid') : t('subscribe.unsubscribeText')}</p>
          {token && !unsubscribe.isError && (
            <Button disabled={!hydrated || unsubscribe.isPending} onClick={() => unsubscribe.mutate(token)}>
              {t('subscribe.unsubscribe')}
            </Button>
          )}
        </>
      )}
      <Link to={localePath(lang)} className="text-primary underline">
        {t('common.backHome')}
      </Link>
    </div>
  )
}
