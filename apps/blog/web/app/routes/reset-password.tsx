import { zodResolver } from '@hookform/resolvers/zod'
import { useForm } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import { Link, useSearchParams } from 'react-router'
import type { z } from 'zod'
import type { Route } from './+types/reset-password'
import { AuthCard } from '~/components/auth-card'
import { FormError, FormField } from '~/components/form'
import { applyFieldErrors } from '~/lib/form-errors'
import { Alert, AlertDescription } from '~/components/ui/alert'
import { Button } from '~/components/ui/button'
import { Input } from '~/components/ui/input'
import { resetForm } from '~/lib/forms'
import { useHydrated } from '~/lib/hydrated'
import { localePath, useLang } from '~/lib/lang'
import { privateMeta } from '~/lib/page'
import { useResetPassword } from '~/lib/queries'

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'auth.resetTitle')

export default function ResetPassword() {
  const hydrated = useHydrated()
  const { t } = useTranslation()
  const lang = useLang()
  const [searchParams] = useSearchParams()
  const reset = useResetPassword()
  const form = useForm<z.infer<typeof resetForm>>({ resolver: zodResolver(resetForm), defaultValues: { password: '' } })
  const token = searchParams.get('token') ?? ''

  return (
    <AuthCard title={t('auth.resetTitle')}>
      {reset.isSuccess ? (
        <div className="grid gap-4">
          <Alert>
            <AlertDescription>{t('auth.resetDone')}</AlertDescription>
          </Alert>
          <Button asChild>
            <Link to={localePath(lang, '/login')}>{t('nav.login')}</Link>
          </Button>
        </div>
      ) : (
        <form
          method="post"
          onSubmit={form.handleSubmit((values) =>
            reset.mutate({ token, password: values.password }, { onError: (error) => applyFieldErrors(form, error) }),
          )}
          className="grid gap-4"
          noValidate
        >
          <FormError error={reset.error} />
          <FormField label={t('auth.newPassword')} hint={t('auth.passwordHint')} error={form.formState.errors.password?.message}>
            {(id, describedBy) => (
              <Input id={id} type="password" autoComplete="new-password" aria-describedby={describedBy} {...form.register('password')} />
            )}
          </FormField>
          <Button type="submit" disabled={!hydrated || reset.isPending || !token}>
            {t('auth.resetSubmit')}
          </Button>
        </form>
      )}
    </AuthCard>
  )
}
