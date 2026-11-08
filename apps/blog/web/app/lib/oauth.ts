// GitHub and Google logins: the providers' names, and why one didn't work
import type { i18n } from 'i18next'

export const providerNames: Record<string, string> = { github: 'GitHub', google: 'Google' }

export function providerName(provider: string | null | undefined) {
  return (provider && providerNames[provider]) ?? provider ?? ''
}

// The reason the server gave when a login with provider came back without working (?oauth_error=)
export function oauthErrorMessage(i18n: i18n, code: string, provider: string | null) {
  const key = `oauth.errors.${code}`
  return i18n.t((i18n.exists(key) ? key : 'oauth.errors.unknown') as 'oauth.errors.unknown', { provider: providerName(provider) })
}
