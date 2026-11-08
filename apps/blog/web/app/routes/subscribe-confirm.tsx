import { useEffect, useRef } from 'react'
import { useTranslation } from 'react-i18next'
import { Link, useSearchParams } from 'react-router'
import type { Route } from './+types/subscribe-confirm'
import { CottageMark } from '~/components/scenery/mark'
import { localePath, useLang } from '~/lib/lang'
import { privateMeta } from '~/lib/page'
import { useConfirmSubscription } from '~/lib/queries'

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'subscribe.confirmTitle')

// The link in the confirmation mail: confirms as soon as the page runs
export default function SubscribeConfirm() {
  const { t } = useTranslation()
  const lang = useLang()
  const [searchParams] = useSearchParams()
  const token = searchParams.get('token') ?? ''
  const confirm = useConfirmSubscription()
  const sent = useRef(false)
  const { mutate } = confirm
  useEffect(() => {
    if (sent.current || !token) return
    sent.current = true
    mutate(token)
  }, [token, mutate])

  return (
    <div className="mx-auto flex max-w-md flex-col items-center gap-4 py-16 text-center">
      <CottageMark className="size-20" />
      <h1 className="font-heading text-2xl font-bold">{t('subscribe.confirmTitle')}</h1>
      <p className="text-muted-foreground">
        {confirm.isSuccess ? t('subscribe.confirmed') : confirm.isError || !token ? t('subscribe.invalid') : t('auth.verifying')}
      </p>
      <Link to={localePath(lang)} className="text-primary underline">
        {t('common.backHome')}
      </Link>
    </div>
  )
}
