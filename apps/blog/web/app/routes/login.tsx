import { zodResolver } from '@hookform/resolvers/zod'
import { useState } from 'react'
import { useForm } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import { Link, useNavigate, useSearchParams } from 'react-router'
import type { z } from 'zod'
import type { Route } from './+types/login'
import { AuthCard } from '~/components/auth-card'
import { FormError, FormField } from '~/components/form'
import { OAuthButtons } from '~/components/oauth-buttons'
import { Alert, AlertDescription } from '~/components/ui/alert'
import { applyFieldErrors } from '~/lib/form-errors'
import { Button } from '~/components/ui/button'
import { Input } from '~/components/ui/input'
import { ApiError } from '~/lib/api'
import { loginForm } from '~/lib/forms'
import { useHydrated } from '~/lib/hydrated'
import { localePath, useLang } from '~/lib/lang'
import { oauthErrorMessage, providerName } from '~/lib/oauth'
import { privateMeta } from '~/lib/page'
import { useLogin, useOAuthTotp } from '~/lib/queries'

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'auth.loginTitle')

// Only paths of our own site after logging in, never another origin
function safeRedirect(target: string | null, fallback: string) {
  return target && target.startsWith('/') && !target.startsWith('//') ? target : fallback
}

// After a GitHub or Google login into an account with two-step login: its code, the ticket is a cookie
function OAuthTotpStep({ provider }: { provider: string | null }) {
  const hydrated = useHydrated()
  const { t } = useTranslation()
  const lang = useLang()
  const navigate = useNavigate()
  const login = useOAuthTotp()
  const [code, setCode] = useState('')
  const expired = login.error instanceof ApiError && login.error.code === 'oauth_expired'

  return (
    <AuthCard title={t('oauth.totpTitle')} description={t('oauth.totpText', { provider: providerName(provider) })}>
      <form
        method="post"
        className="grid gap-4"
        noValidate
        onSubmit={(event) => {
          event.preventDefault()
          login.mutate(code.trim(), {
            onSuccess: (me) => void navigate(safeRedirect(me.next, localePath(lang)), { replace: true }),
            onError: () => setCode(''),
          })
        }}
      >
        <FormError error={login.error} />
        {expired ? (
          <Button type="button" variant="outline" onClick={() => void navigate(localePath(lang, '/login'), { replace: true })}>
            {t('auth.loginTitle')}
          </Button>
        ) : (
          <>
            <FormField label={t('auth.totp')} hint={t('auth.totpHint')}>
              {(id, describedBy) => (
                <Input
                  id={id}
                  inputMode="numeric"
                  autoComplete="one-time-code"
                  maxLength={6}
                  autoFocus
                  value={code}
                  onChange={(event) => setCode(event.target.value)}
                  aria-describedby={describedBy}
                />
              )}
            </FormField>
            <Button type="submit" disabled={!hydrated || login.isPending || code.trim().length !== 6}>
              {t('oauth.totpSubmit')}
            </Button>
          </>
        )}
      </form>
    </AuthCard>
  )
}

export default function Login() {
  const hydrated = useHydrated()
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const navigate = useNavigate()
  const [searchParams] = useSearchParams()
  const login = useLogin()
  const [needTotp, setNeedTotp] = useState(false)
  // Back from GitHub or Google
  const oauthError = searchParams.get('oauth_error')
  const oauthProvider = searchParams.get('provider')
  const form = useForm<z.infer<typeof loginForm>>({
    resolver: zodResolver(loginForm),
    defaultValues: { login: '', password: '', totp: '' },
  })

  const onSubmit = form.handleSubmit((values) =>
    login.mutate(
      { login: values.login, password: values.password, totp: needTotp ? values.totp : undefined },
      {
        onSuccess: () => void navigate(safeRedirect(searchParams.get('next'), localePath(lang)), { replace: true }),
        onError: (error) => {
          if (error instanceof ApiError && (error.code === 'totp_required' || error.code === 'totp_invalid')) setNeedTotp(true)
          applyFieldErrors(form, error)
        },
      },
    ),
  )

  if (searchParams.get('oauth') === 'totp') return <OAuthTotpStep provider={oauthProvider} />

  return (
    <AuthCard
      title={t('auth.loginTitle')}
      footer={
        <span>
          {t('auth.noAccount')}{' '}
          <Link to={localePath(lang, '/register')} className="text-foreground underline">
            {t('nav.register')}
          </Link>
        </span>
      }
    >
      <form method="post" onSubmit={onSubmit} className="grid gap-4" noValidate>
        {oauthError && !login.error && (
          <Alert variant="destructive">
            <AlertDescription>{oauthErrorMessage(i18n, oauthError, oauthProvider)}</AlertDescription>
          </Alert>
        )}
        <FormError error={login.error instanceof ApiError && login.error.code === 'totp_required' ? null : login.error} />
        <FormField label={t('auth.login')} error={form.formState.errors.login?.message}>
          {(id, describedBy) => (
            <Input
              id={id}
              autoComplete="username"
              aria-describedby={describedBy}
              aria-invalid={!!form.formState.errors.login}
              {...form.register('login')}
            />
          )}
        </FormField>
        <FormField label={t('auth.password')} error={form.formState.errors.password?.message}>
          {(id, describedBy) => (
            <Input
              id={id}
              type="password"
              autoComplete="current-password"
              aria-describedby={describedBy}
              aria-invalid={!!form.formState.errors.password}
              {...form.register('password')}
            />
          )}
        </FormField>
        {needTotp && (
          <FormField label={t('auth.totp')} hint={t('auth.totpHint')} error={form.formState.errors.totp?.message}>
            {(id, describedBy) => (
              <Input
                id={id}
                inputMode="numeric"
                autoComplete="one-time-code"
                maxLength={6}
                autoFocus
                aria-describedby={describedBy}
                {...form.register('totp')}
              />
            )}
          </FormField>
        )}
        <Button type="submit" disabled={!hydrated || login.isPending}>
          {t('auth.submitLogin')}
        </Button>
        <Link to={localePath(lang, '/forgot-password')} className="text-center text-sm text-muted-foreground hover:text-foreground">
          {t('auth.forgot')}
        </Link>
      </form>
      <OAuthButtons action="login" next={safeRedirect(searchParams.get('next'), localePath(lang))} />
    </AuthCard>
  )
}
