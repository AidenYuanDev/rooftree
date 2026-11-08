import { MailIcon } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { CaptchaImage } from '~/components/captcha-image'
import { errorMessage, fieldErrorMessage } from '~/i18n'
import { ApiError } from '~/lib/api'
import { useHydrated } from '~/lib/hydrated'
import { useLang } from '~/lib/lang'
import { useCaptcha, useSubscribe } from '~/lib/queries'
import { cn } from '~/lib/utils'

// An address to mail new writing to; the server mails a link to confirm it first. The form is on every
// page: its captcha is only asked for once someone starts typing an address
export function SubscribeForm({ className, tone = 'page' }: { className?: string; tone?: 'page' | 'footer' }) {
  const { t, i18n } = useTranslation()
  const hydrated = useHydrated()
  const lang = useLang()
  const subscribe = useSubscribe()
  const [email, setEmail] = useState('')
  const [started, setStarted] = useState(false)
  const captcha = useCaptcha(started)
  const [answer, setAnswer] = useState('')

  if (subscribe.isSuccess) return <p className={cn('text-sm', className)}>{t('subscribe.sent')}</p>

  const fields = subscribe.error instanceof ApiError ? subscribe.error.fields : {}
  const inputClass = cn(
    'h-9 min-w-0 flex-1 rounded-full border px-4 text-sm outline-none focus-visible:ring-2 focus-visible:ring-ring/50',
    tone === 'footer' ? 'border-white/15 bg-white/5 text-white placeholder:text-white/40' : 'bg-card/70',
  )

  return (
    <form
      method="post"
      className={cn('grid grid-cols-1 gap-2', className)}
      onSubmit={(event) => {
        event.preventDefault()
        subscribe.mutate(
          { email, lang, captcha: answer, captchaId: captcha.id },
          {
            // The server takes each picture once
            onError: () => {
              setAnswer('')
              captcha.refresh()
            },
          },
        )
      }}
    >
      <div className="flex gap-2">
        <input
          type="email"
          required
          value={email}
          onChange={(event) => {
            setEmail(event.target.value)
            setStarted(true)
          }}
          onFocus={() => setStarted(true)}
          placeholder={t('subscribe.placeholder')}
          aria-label={t('subscribe.email')}
          className={inputClass}
        />
        <button
          type="submit"
          disabled={!hydrated || subscribe.isPending}
          className="flex h-9 shrink-0 items-center gap-1.5 rounded-full bg-primary px-4 text-sm text-primary-foreground transition hover:opacity-90 disabled:opacity-60"
        >
          <MailIcon className="size-4" />
          {t('subscribe.submit')}
        </button>
      </div>
      {started && captcha.enabled && (
        <div className="flex gap-2">
          <input
            value={answer}
            onChange={(event) => setAnswer(event.target.value)}
            autoComplete="off"
            autoCapitalize="characters"
            spellCheck={false}
            maxLength={16}
            placeholder={t('captcha.placeholder')}
            aria-label={t('captcha.label')}
            aria-invalid={!!fields.captcha}
            className={inputClass}
          />
          <CaptchaImage captcha={captcha} className="h-9 w-[103px] rounded-full" />
        </div>
      )}
      {subscribe.error && (
        <p className="text-xs text-destructive">
          {fields.captcha && !fields.email
            ? `${t('captcha.label')}: ${fieldErrorMessage(i18n, fields.captcha)}`
            : errorMessage(i18n, subscribe.error)}
        </p>
      )}
    </form>
  )
}
