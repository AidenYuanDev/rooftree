import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import { Button } from '~/components/ui/button'
import { errorMessage } from '~/i18n'
import { ApiError } from '~/lib/api'
import { useMe, useResendVerification } from '~/lib/queries'
import { useCountdown } from '~/lib/use-countdown'

// The server's Blog.Auth.MailInterval by default: one mail to an address a minute
const MailInterval = 60

export function UnverifiedBanner() {
  const { t, i18n } = useTranslation()
  const user = useMe().data?.user
  const resend = useResendVerification()
  const [wait, startWait] = useCountdown()
  if (!user || user.emailVerified !== false) return null

  return (
    <div className="border-b bg-amber-50 text-amber-900 dark:bg-amber-950 dark:text-amber-100">
      <div className="mx-auto flex max-w-5xl flex-wrap items-center gap-2 px-4 py-2 text-sm">
        <span className="mr-auto">{t('auth.unverifiedBanner')}</span>
        <Button
          size="xs"
          variant="outline"
          disabled={resend.isPending || wait > 0}
          onClick={() =>
            resend.mutate(undefined, {
              onSuccess: () => {
                toast.success(t('auth.resent'))
                startWait(MailInterval)
              },
              onError: (e) => {
                toast.error(errorMessage(i18n, e))
                if (e instanceof ApiError && e.retryAfter) startWait(e.retryAfter)
              },
            })
          }
        >
          {wait > 0 ? t('auth.resendIn', { seconds: wait }) : t('auth.resend')}
        </Button>
      </div>
    </div>
  )
}
