import { useTranslation } from 'react-i18next'
import { Alert, AlertDescription } from '~/components/ui/alert'
import { Button } from '~/components/ui/button'
import { errorMessage } from '~/i18n'
import { useHydrated } from '~/lib/hydrated'
import { useLang } from '~/lib/lang'
import { providerName } from '~/lib/oauth'
import { useOAuthProviders, useStartOAuth } from '~/lib/queries'

export function ProviderIcon({ provider, className = 'size-4' }: { provider: string; className?: string }) {
  if (provider === 'github')
    return (
      <svg viewBox="0 0 24 24" aria-hidden className={className} fill="currentColor">
        <path d="M12 .5a11.5 11.5 0 0 0-3.64 22.41c.58.1.79-.25.79-.56v-2c-3.2.7-3.88-1.36-3.88-1.36-.52-1.33-1.28-1.69-1.28-1.69-1.05-.71.08-.7.08-.7 1.16.08 1.77 1.19 1.77 1.19 1.03 1.77 2.7 1.26 3.36.96.1-.75.4-1.26.73-1.55-2.55-.29-5.24-1.28-5.24-5.69 0-1.26.45-2.29 1.19-3.09-.12-.29-.52-1.46.11-3.05 0 0 .97-.31 3.17 1.18a11 11 0 0 1 5.77 0c2.2-1.49 3.17-1.18 3.17-1.18.63 1.59.23 2.76.11 3.05.74.8 1.19 1.83 1.19 3.09 0 4.42-2.69 5.39-5.26 5.68.41.36.78 1.06.78 2.14v3.17c0 .31.21.67.8.56A11.5 11.5 0 0 0 12 .5Z" />
      </svg>
    )
  if (provider === 'google')
    return (
      <svg viewBox="0 0 24 24" aria-hidden className={className}>
        <path
          fill="#4285F4"
          d="M23.5 12.27c0-.85-.08-1.67-.22-2.45H12v4.64h6.45a5.52 5.52 0 0 1-2.39 3.62v3h3.87c2.26-2.09 3.57-5.16 3.57-8.81Z"
        />
        <path
          fill="#34A853"
          d="M12 24c3.24 0 5.96-1.07 7.94-2.91l-3.87-3c-1.08.72-2.45 1.15-4.07 1.15-3.13 0-5.78-2.11-6.73-4.95H1.27v3.1A12 12 0 0 0 12 24Z"
        />
        <path fill="#FBBC05" d="M5.27 14.29a7.2 7.2 0 0 1 0-4.58V6.61h-4a12 12 0 0 0 0 10.78l4-3.1Z" />
        <path
          fill="#EA4335"
          d="M12 4.77c1.76 0 3.34.61 4.59 1.8l3.43-3.43A11.5 11.5 0 0 0 12 0 12 12 0 0 0 1.27 6.61l4 3.1C6.22 6.88 8.87 4.77 12 4.77Z"
        />
      </svg>
    )
  return null
}

// "or" and a button per provider the server has set up; nothing when it has none. next: the page to
// come back to after logging in
export function OAuthButtons({ action, next }: { action: 'login' | 'register'; next?: string }) {
  const { t, i18n } = useTranslation()
  const hydrated = useHydrated()
  const lang = useLang()
  const providers = useOAuthProviders().data?.providers ?? []
  const start = useStartOAuth()
  if (!providers.length) return null

  return (
    <div className="mt-4 grid gap-3">
      <div className="flex items-center gap-3 text-xs text-muted-foreground">
        <span className="h-px flex-1 bg-border" />
        {t('oauth.or')}
        <span className="h-px flex-1 bg-border" />
      </div>
      {start.error && (
        <Alert variant="destructive">
          <AlertDescription>{errorMessage(i18n, start.error)}</AlertDescription>
        </Alert>
      )}
      {providers.map((provider) => (
        <Button
          key={provider}
          type="button"
          variant="outline"
          // Stays disabled while the browser leaves for the provider
          disabled={!hydrated || start.isPending || start.isSuccess}
          onClick={() => start.mutate({ provider, lang, next })}
        >
          <ProviderIcon provider={provider} />
          {t(action === 'login' ? 'oauth.login' : 'oauth.register', { provider: providerName(provider) })}
        </Button>
      ))}
    </div>
  )
}
