import { zodResolver } from '@hookform/resolvers/zod'
import { useForm } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import type { z } from 'zod'
import type { Route } from './+types/forgot-password'
import { AuthCard } from '~/components/auth-card'
import { CaptchaField } from '~/components/captcha-image'
import { FormError, FormField } from '~/components/form'
import { applyFieldErrors } from '~/lib/form-errors'
import { Alert, AlertDescription } from '~/components/ui/alert'
import { Button } from '~/components/ui/button'
import { Input } from '~/components/ui/input'
import { forgotForm } from '~/lib/forms'
import { useHydrated } from '~/lib/hydrated'
import { privateMeta } from '~/lib/page'
import { useCaptcha, useForgotPassword } from '~/lib/queries'

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'auth.forgotTitle')

export default function ForgotPassword() {
  const hydrated = useHydrated()
  const { t } = useTranslation()
  const forgot = useForgotPassword()
  const captcha = useCaptcha()
  const form = useForm<z.infer<typeof forgotForm>>({ resolver: zodResolver(forgotForm), defaultValues: { email: '', captcha: '' } })

  return (
    <AuthCard title={t('auth.forgotTitle')} description={forgot.isSuccess ? undefined : t('auth.forgotIntro')}>
      {forgot.isSuccess ? (
        <Alert>
          <AlertDescription>{t('auth.forgotSent')}</AlertDescription>
        </Alert>
      ) : (
        <form
          method="post"
          onSubmit={form.handleSubmit((values) =>
            forgot.mutate(
              { ...values, captchaId: captcha.id },
              {
                onError: (error) => {
                  applyFieldErrors(form, error)
                  form.setValue('captcha', '')
                  captcha.refresh()
                },
              },
            ),
          )}
          className="grid gap-4"
          noValidate
        >
          <FormError error={forgot.error} />
          <FormField label={t('auth.email')} error={form.formState.errors.email?.message}>
            {(id, describedBy) => (
              <Input id={id} type="email" autoComplete="email" aria-describedby={describedBy} {...form.register('email')} />
            )}
          </FormField>
          <CaptchaField captcha={captcha} error={form.formState.errors.captcha?.message} input={form.register('captcha')} />
          <Button type="submit" disabled={!hydrated || forgot.isPending}>
            {t('auth.forgotSubmit')}
          </Button>
        </form>
      )}
    </AuthCard>
  )
}
