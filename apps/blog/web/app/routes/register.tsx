import { zodResolver } from '@hookform/resolvers/zod'
import { useForm } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import type { z } from 'zod'
import type { Route } from './+types/register'
import { AuthCard } from '~/components/auth-card'
import { CaptchaField } from '~/components/captcha-image'
import { OAuthButtons } from '~/components/oauth-buttons'
import { FormError, FormField } from '~/components/form'
import { applyFieldErrors } from '~/lib/form-errors'
import { Alert, AlertDescription } from '~/components/ui/alert'
import { Button } from '~/components/ui/button'
import { Input } from '~/components/ui/input'
import { ApiError } from '~/lib/api'
import { registerForm } from '~/lib/forms'
import { useHydrated } from '~/lib/hydrated'
import { localePath, useLang } from '~/lib/lang'
import { privateMeta } from '~/lib/page'
import { useCaptcha, useRegister } from '~/lib/queries'

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'auth.registerTitle')

export default function Register() {
  const hydrated = useHydrated()
  const { t } = useTranslation()
  const lang = useLang()
  const register = useRegister()
  const captcha = useCaptcha()
  const form = useForm<z.infer<typeof registerForm>>({
    resolver: zodResolver(registerForm),
    defaultValues: { username: '', displayName: '', email: '', password: '', locale: lang, captcha: '' },
  })
  const errors = form.formState.errors

  // The server takes each picture once: a new one after every answer that didn't go through
  const onSubmit = form.handleSubmit((values) =>
    register.mutate(
      { ...values, captchaId: captcha.id },
      {
        onError: (error) => {
          applyFieldErrors(form, error)
          form.setValue('captcha', '')
          captcha.refresh()
        },
      },
    ),
  )

  if (register.isSuccess) {
    return (
      <AuthCard title={t('auth.registerTitle')}>
        <Alert>
          <AlertDescription>{t('auth.registered', { email: form.getValues('email') })}</AlertDescription>
        </Alert>
      </AuthCard>
    )
  }

  // Field errors show at their fields, only other errors on top
  const generalError = register.error instanceof ApiError && Object.keys(register.error.fields).length ? null : register.error
  return (
    <AuthCard
      title={t('auth.registerTitle')}
      footer={
        <span>
          {t('auth.haveAccount')}{' '}
          <Link to={localePath(lang, '/login')} className="text-foreground underline">
            {t('nav.login')}
          </Link>
        </span>
      }
    >
      <form method="post" onSubmit={onSubmit} className="grid gap-4" noValidate>
        <FormError error={generalError} />
        <FormField label={t('auth.username')} hint={t('auth.usernameHint')} error={errors.username?.message}>
          {(id, describedBy) => (
            <Input
              id={id}
              autoComplete="username"
              aria-describedby={describedBy}
              aria-invalid={!!errors.username}
              {...form.register('username')}
            />
          )}
        </FormField>
        <FormField label={t('auth.displayName')} error={errors.displayName?.message}>
          {(id, describedBy) => (
            <Input
              id={id}
              autoComplete="nickname"
              aria-describedby={describedBy}
              aria-invalid={!!errors.displayName}
              {...form.register('displayName')}
            />
          )}
        </FormField>
        <FormField label={t('auth.email')} hint={t('auth.emailHint')} error={errors.email?.message}>
          {(id, describedBy) => (
            <Input
              id={id}
              type="email"
              autoComplete="email"
              aria-describedby={describedBy}
              aria-invalid={!!errors.email}
              {...form.register('email')}
            />
          )}
        </FormField>
        <FormField label={t('auth.password')} hint={t('auth.passwordHint')} error={errors.password?.message}>
          {(id, describedBy) => (
            <Input
              id={id}
              type="password"
              autoComplete="new-password"
              aria-describedby={describedBy}
              aria-invalid={!!errors.password}
              {...form.register('password')}
            />
          )}
        </FormField>
        <CaptchaField captcha={captcha} error={errors.captcha?.message} input={form.register('captcha')} />
        <Button type="submit" disabled={!hydrated || register.isPending}>
          {t('auth.submitRegister')}
        </Button>
      </form>
      <OAuthButtons action="register" next={localePath(lang)} />
    </AuthCard>
  )
}
