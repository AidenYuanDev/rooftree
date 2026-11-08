import { useEffect, useRef } from 'react'
import { useTranslation } from 'react-i18next'
import { Link, useSearchParams } from 'react-router'
import type { Route } from './+types/verify-email'
import { AuthCard } from '~/components/auth-card'
import { Alert, AlertDescription } from '~/components/ui/alert'
import { Button } from '~/components/ui/button'
import { localePath, useLang } from '~/lib/lang'
import { privateMeta } from '~/lib/page'
import { useVerifyEmail } from '~/lib/queries'

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'auth.verifyTitle')

export default function VerifyEmail() {
  const { t } = useTranslation()
  const lang = useLang()
  const [searchParams] = useSearchParams()
  const verify = useVerifyEmail()
  const token = searchParams.get('token') ?? ''
  // The token is used up by the first request: once, also under StrictMode's double effects
  const sent = useRef(false)

  useEffect(() => {
    if (sent.current || !token) return
    sent.current = true
    verify.mutate(token)
  }, [token, verify])

  return (
    <AuthCard title={t('auth.verifyTitle')}>
      <div className="grid gap-4">
        {(verify.isPending || verify.isIdle) && token && <p className="text-muted-foreground">{t('auth.verifying')}</p>}
        {verify.isSuccess && (
          <Alert>
            <AlertDescription>{t('auth.verified')}</AlertDescription>
          </Alert>
        )}
        {(verify.isError || !token) && (
          <Alert variant="destructive">
            <AlertDescription>{t('auth.verifyFailed')}</AlertDescription>
          </Alert>
        )}
        <Button asChild variant="outline">
          <Link to={localePath(lang)}>{t('common.backHome')}</Link>
        </Button>
      </div>
    </AuthCard>
  )
}
