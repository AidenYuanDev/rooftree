import { zodResolver } from '@hookform/resolvers/zod'
import { CheckIcon, MessageSquareIcon, UserMinusIcon } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { useForm, useWatch } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import { Link, useNavigate, useSearchParams } from 'react-router'
import { toast } from 'sonner'
import type { z } from 'zod'
import type { Route } from './+types/account'
import { FormError, FormField } from '~/components/form'
import { ProviderIcon } from '~/components/oauth-buttons'
import { applyFieldErrors } from '~/lib/form-errors'
import { RequireUser } from '~/components/require-user'
import { UserAvatar } from '~/components/user-avatar'
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '~/components/ui/alert-dialog'
import { Button } from '~/components/ui/button'
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '~/components/ui/card'
import { Input } from '~/components/ui/input'
import { Label } from '~/components/ui/label'
import { Switch } from '~/components/ui/switch'
import { Tabs, TabsContent, TabsList, TabsTrigger } from '~/components/ui/tabs'
import { errorMessage, fieldErrorMessage } from '~/i18n'
import { ApiError } from '~/lib/api'
import { squareAvatar } from '~/lib/avatar'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '~/components/ui/select'
import { changePasswordForm, passwordOnlyForm, profileForm, setPasswordForm, totpCodeForm } from '~/lib/forms'
import { localePath, switchLang, useLang } from '~/lib/lang'
import { oauthErrorMessage, providerName } from '~/lib/oauth'
import { useHydrated } from '~/lib/hydrated'
import { privateMeta } from '~/lib/page'
import {
  useAcceptFriendRequest,
  useChangePassword,
  useIdentities,
  useOAuthProviders,
  useRejectFriendRequest,
  useRemoveFriend,
  useSetAvatar,
  useSetSubscription,
  useStartOAuth,
  useSubscription,
  useTotpDisable,
  useTotpEnable,
  useTotpSetup,
  useUnlinkIdentity,
  useUpdateProfile,
  useUserFriends,
  useUserFriendRequests,
} from '~/lib/queries'
import { type Lang, langs, type User, type UserFriendItem } from '~/lib/schemas'
import { useChatStore } from '~/stores/chat'

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'account.title')

// The picture: an upload (cropped and encoded in the browser), a linked provider's, or the initials
function AvatarPicker({ user }: { user: User }) {
  const { t, i18n } = useTranslation()
  const hydrated = useHydrated()
  const providers = useOAuthProviders().data?.providers ?? []
  const identities = useIdentities(providers.length > 0)
  const setAvatar = useSetAvatar()
  const input = useRef<HTMLInputElement>(null)

  const onError = (error: unknown) => {
    const field = error instanceof ApiError ? error.fields.file : undefined
    toast.error(field ? fieldErrorMessage(i18n, field) : errorMessage(i18n, error))
  }
  const onSuccess = () => toast.success(t('account.avatarSaved'))

  const onFile = async (file: File | undefined) => {
    if (input.current) input.current.value = ''
    if (!file) return
    let picture: Blob
    try {
      picture = await squareAvatar(file)
    } catch {
      return toast.error(t('account.avatarNotImage'))
    }
    setAvatar.mutate({ picture }, { onSuccess, onError })
  }

  const withPictures = (identities.data?.identities ?? []).filter((identity) => identity.hasAvatar)
  const busy = !hydrated || setAvatar.isPending
  return (
    <div className="flex flex-wrap items-center gap-4">
      <UserAvatar user={user} className="size-16 text-2xl" />
      <div className="grid gap-2">
        <div className="flex flex-wrap gap-2">
          <Button type="button" size="sm" variant="outline" disabled={busy} onClick={() => input.current?.click()}>
            {t('account.avatarUpload')}
          </Button>
          {withPictures.map((identity) => (
            <Button
              key={identity.provider}
              type="button"
              size="sm"
              variant="outline"
              disabled={busy}
              onClick={() => setAvatar.mutate({ provider: identity.provider }, { onSuccess, onError })}
            >
              <ProviderIcon provider={identity.provider} />
              {t('account.avatarFrom', { provider: providerName(identity.provider) })}
            </Button>
          ))}
          {user.avatar && (
            <Button
              type="button"
              size="sm"
              variant="ghost"
              disabled={busy}
              onClick={() => setAvatar.mutate({ remove: true }, { onSuccess: () => toast.success(t('account.avatarRemoved')), onError })}
            >
              {t('account.avatarRemove')}
            </Button>
          )}
        </div>
        <p className="text-xs text-muted-foreground">{t('account.avatarHint')}</p>
      </div>
      <input ref={input} type="file" accept="image/*" hidden onChange={(event) => void onFile(event.target.files?.[0])} />
    </div>
  )
}

function ProfileCard({ user }: { user: User }) {
  const hydrated = useHydrated()
  const { t } = useTranslation()
  const lang = useLang()
  const navigate = useNavigate()
  const update = useUpdateProfile()
  const form = useForm<z.infer<typeof profileForm>>({
    resolver: zodResolver(profileForm),
    defaultValues: { displayName: user.displayName, locale: user.locale ?? lang },
  })

  const locale = useWatch({ control: form.control, name: 'locale' })

  const onSubmit = form.handleSubmit((values) =>
    update.mutate(values, {
      onSuccess: () => {
        toast.success(t('account.saved'))
        // The interface follows the chosen language
        if (values.locale !== lang) void navigate(switchLang(window.location.pathname, values.locale))
      },
      onError: (error) => applyFieldErrors(form, error),
    }),
  )

  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('account.profile')}</CardTitle>
        <CardDescription>
          @{user.username} · {user.email}
        </CardDescription>
      </CardHeader>
      <CardContent className="grid gap-6">
        <AvatarPicker user={user} />
        <form method="post" onSubmit={onSubmit} className="grid gap-4" noValidate>
          <FormError error={update.error} />
          <FormField label={t('auth.displayName')} error={form.formState.errors.displayName?.message}>
            {(id, describedBy) => <Input id={id} aria-describedby={describedBy} {...form.register('displayName')} />}
          </FormField>
          <FormField label={t('account.locale')}>
            {(id) => (
              <Select value={locale} onValueChange={(value) => form.setValue('locale', value as Lang, { shouldDirty: true })}>
                <SelectTrigger id={id} className="w-full">
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  {langs.map((l) => (
                    <SelectItem key={l} value={l}>
                      {t(`langs.${l}`)}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            )}
          </FormField>
          <Button type="submit" className="justify-self-start" disabled={!hydrated || update.isPending}>
            {t('account.save')}
          </Button>
        </form>
      </CardContent>
    </Card>
  )
}

// Changing the password, or setting the first one of an account made by a GitHub or Google login
function PasswordCard({ user }: { user: User }) {
  const hydrated = useHydrated()
  const { t } = useTranslation()
  const change = useChangePassword()
  const hasPassword = user.hasPassword !== false
  const form = useForm<z.infer<typeof changePasswordForm>>({
    resolver: zodResolver(hasPassword ? changePasswordForm : setPasswordForm),
    defaultValues: { currentPassword: '', newPassword: '' },
  })

  const onSubmit = form.handleSubmit((values) =>
    change.mutate(hasPassword ? values : { newPassword: values.newPassword }, {
      onSuccess: () => {
        toast.success(t(hasPassword ? 'account.passwordChanged' : 'account.passwordSet'))
        form.reset()
      },
      onError: (error) => applyFieldErrors(form, error),
    }),
  )

  return (
    <Card>
      <CardHeader>
        <CardTitle>{t(hasPassword ? 'account.passwordTitle' : 'account.setPasswordTitle')}</CardTitle>
        {!hasPassword && <CardDescription>{t('account.setPasswordText')}</CardDescription>}
      </CardHeader>
      <CardContent>
        <form method="post" onSubmit={onSubmit} className="grid gap-4" noValidate>
          {hasPassword && (
            <FormField label={t('account.currentPassword')} error={form.formState.errors.currentPassword?.message}>
              {(id, describedBy) => (
                <Input
                  id={id}
                  type="password"
                  autoComplete="current-password"
                  aria-describedby={describedBy}
                  {...form.register('currentPassword')}
                />
              )}
            </FormField>
          )}
          <FormField label={t('auth.newPassword')} hint={t('auth.passwordHint')} error={form.formState.errors.newPassword?.message}>
            {(id, describedBy) => (
              <Input id={id} type="password" autoComplete="new-password" aria-describedby={describedBy} {...form.register('newPassword')} />
            )}
          </FormField>
          <Button type="submit" className="justify-self-start" disabled={!hydrated || change.isPending}>
            {t(hasPassword ? 'account.changePassword' : 'account.setPassword')}
          </Button>
        </form>
      </CardContent>
    </Card>
  )
}

function TotpCard({ user }: { user: User }) {
  const hydrated = useHydrated()
  const { t } = useTranslation()
  const setup = useTotpSetup()
  const enable = useTotpEnable()
  const disable = useTotpDisable()
  const codeForm = useForm<z.infer<typeof totpCodeForm>>({ resolver: zodResolver(totpCodeForm), defaultValues: { code: '' } })
  const passwordForm = useForm<z.infer<typeof passwordOnlyForm>>({
    resolver: zodResolver(passwordOnlyForm),
    defaultValues: { password: '' },
  })

  let content: React.ReactNode
  if (user.totpEnabled) {
    content =
      user.hasPassword === false ? (
        <p className="text-sm text-muted-foreground">{t('account.totpNeedsPassword')}</p>
      ) : (
        <form
          method="post"
          onSubmit={passwordForm.handleSubmit((values) =>
            disable.mutate(values.password, {
              onSuccess: () => toast.success(t('account.totpDisabled')),
              onError: (error) => applyFieldErrors(passwordForm, error),
            }),
          )}
          className="grid gap-4"
          noValidate
        >
          <FormField label={t('auth.password')} error={passwordForm.formState.errors.password?.message}>
            {(id, describedBy) => (
              <Input
                id={id}
                type="password"
                autoComplete="current-password"
                aria-describedby={describedBy}
                {...passwordForm.register('password')}
              />
            )}
          </FormField>
          <Button type="submit" variant="destructive" className="justify-self-start" disabled={!hydrated || disable.isPending}>
            {t('account.totpDisable')}
          </Button>
        </form>
      )
  } else if (setup.data) {
    content = (
      <form
        method="post"
        onSubmit={codeForm.handleSubmit((values) =>
          enable.mutate(values.code, {
            onSuccess: () => {
              toast.success(t('account.totpEnabled'))
              setup.reset()
            },
            onError: (error) => applyFieldErrors(codeForm, error),
          }),
        )}
        className="grid gap-4"
        noValidate
      >
        <p className="text-sm">{t('account.totpScan')}</p>
        <code className="rounded bg-muted p-2 text-sm break-all select-all">{setup.data.otpauthUri}</code>
        <p className="text-sm">
          {t('account.totpSecret')}: <code className="font-mono select-all">{setup.data.secret}</code>
        </p>
        <FormField label={t('auth.totp')} error={codeForm.formState.errors.code?.message}>
          {(id, describedBy) => (
            <Input
              id={id}
              inputMode="numeric"
              autoComplete="one-time-code"
              maxLength={6}
              aria-describedby={describedBy}
              {...codeForm.register('code')}
            />
          )}
        </FormField>
        <Button type="submit" className="justify-self-start" disabled={!hydrated || enable.isPending}>
          {t('account.totpEnable')}
        </Button>
      </form>
    )
  } else {
    content = (
      <div className="grid gap-4">
        <FormError error={setup.error} />
        <Button className="justify-self-start" onClick={() => setup.mutate()} disabled={setup.isPending}>
          {t('account.totpSetup')}
        </Button>
      </div>
    )
  }

  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('account.totpTitle')}</CardTitle>
        <CardDescription>{user.totpEnabled ? t('account.totpOn') : t('account.totpOff')}</CardDescription>
      </CardHeader>
      <CardContent>{content}</CardContent>
    </Card>
  )
}

// GitHub and Google: linked ones with their login, the others to link. Not for admins, nor when the
// server has none set up
function IdentitiesCard() {
  const { t, i18n } = useTranslation()
  const hydrated = useHydrated()
  const lang = useLang()
  const providers = useOAuthProviders().data?.providers ?? []
  const identities = useIdentities(providers.length > 0)
  const start = useStartOAuth()
  const unlink = useUnlinkIdentity()
  const [searchParams, setSearchParams] = useSearchParams()

  // Back from linking at the provider: say how it went once, then drop it from the address
  useEffect(() => {
    const linked = searchParams.get('oauth_linked')
    const error = searchParams.get('oauth_error')
    if (!linked && !error) return
    if (linked) toast.success(t('oauth.linked', { provider: providerName(linked) }))
    else if (error) toast.error(oauthErrorMessage(i18n, error, searchParams.get('provider')))
    setSearchParams({}, { replace: true })
  }, [searchParams, setSearchParams, t, i18n])

  if (!providers.length) return null
  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('oauth.identitiesTitle')}</CardTitle>
        <CardDescription>{t('oauth.identitiesText')}</CardDescription>
      </CardHeader>
      <CardContent className="grid gap-3">
        <FormError error={start.error} />
        {providers.map((provider) => {
          const identity = identities.data?.identities.find((i) => i.provider === provider)
          return (
            <div key={provider} className="flex items-center gap-3">
              <ProviderIcon provider={provider} className="size-5 shrink-0" />
              <div className="min-w-0 flex-1">
                <div className="text-sm font-medium">{providerName(provider)}</div>
                <div className="truncate text-xs text-muted-foreground">{identity ? identity.login : t('oauth.notLinked')}</div>
              </div>
              {identity ? (
                <Button
                  size="sm"
                  variant="outline"
                  disabled={!hydrated || unlink.isPending}
                  onClick={() =>
                    unlink.mutate(provider, {
                      onSuccess: () => toast.success(t('oauth.unlinked', { provider: providerName(provider) })),
                      onError: (error) => toast.error(errorMessage(i18n, error)),
                    })
                  }
                >
                  {t('oauth.unlink')}
                </Button>
              ) : (
                <Button
                  size="sm"
                  disabled={!hydrated || !identities.data || start.isPending || start.isSuccess}
                  onClick={() => start.mutate({ provider, lang, link: true, next: `/${lang}/account` })}
                >
                  {t('oauth.link')}
                </Button>
              )}
            </div>
          )
        })}
      </CardContent>
    </Card>
  )
}

function SubscriptionCard({ user }: { user: User }) {
  const { t, i18n } = useTranslation()
  const subscription = useSubscription(true)
  const set = useSetSubscription()
  const profile = useUpdateProfile()
  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('account.subscriptionTitle')}</CardTitle>
        <CardDescription>{user.emailVerified ? t('account.subscriptionText') : t('reactions.confirmEmail')}</CardDescription>
      </CardHeader>
      <CardContent className="flex items-center gap-2">
        <Switch
          id="subscribed"
          checked={subscription.data?.subscribed ?? false}
          disabled={!user.emailVerified || subscription.isPending || set.isPending}
          onCheckedChange={(checked) =>
            set.mutate(checked, {
              onSuccess: (state) => toast.success(state.subscribed ? t('account.subscribed') : t('account.unsubscribed')),
              onError: (error) => toast.error(errorMessage(i18n, error)),
            })
          }
        />
        <Label htmlFor="subscribed">{t('account.subscriptionSwitch')}</Label>
      </CardContent>
      <CardContent className="flex items-center gap-2">
        <Switch
          id="reply-mails"
          checked={user.replyMails ?? true}
          disabled={!user.emailVerified || profile.isPending}
          onCheckedChange={(replyMails) =>
            profile.mutate(
              { replyMails },
              {
                onSuccess: () => toast.success(replyMails ? t('account.replyMailsOn') : t('account.replyMailsOff')),
                onError: (error) => toast.error(errorMessage(i18n, error)),
              },
            )
          }
        />
        <Label htmlFor="reply-mails">{t('account.replyMails')}</Label>
      </CardContent>
    </Card>
  )
}

function FriendsCard({ user }: { user: User }) {
  const { t } = useTranslation()
  const lang = useLang()
  const friendsQuery = useUserFriends(true)
  const requestsQuery = useUserFriendRequests(true)
  const acceptRequest = useAcceptFriendRequest()
  const rejectRequest = useRejectFriendRequest()
  const removeFriend = useRemoveFriend()

  const [toRemove, setToRemove] = useState<UserFriendItem | null>(null)

  const friends = friendsQuery.data?.friends ?? []
  const incoming = requestsQuery.data?.incoming ?? []
  const outgoing = requestsQuery.data?.outgoing ?? []
  const incomingCount = incoming.length

  const onAccept = (id: number) => {
    acceptRequest.mutate(id, {
      onSuccess: () => toast.success(t('userFriends.requestAccepted')),
    })
  }

  const onReject = (id: number) => {
    rejectRequest.mutate(id, {
      onSuccess: () => toast.success(t('userFriends.requestRejected')),
    })
  }

  const onCancel = (id: number) => {
    rejectRequest.mutate(id, {
      onSuccess: () => toast.success(t('userFriends.requestCanceled')),
    })
  }

  const onUnfriend = () => {
    if (!toRemove) return
    removeFriend.mutate(toRemove.id, {
      onSuccess: () => {
        toast.success(t('userFriends.unfriended'))
        setToRemove(null)
      },
    })
  }

  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('userFriends.title')}</CardTitle>
        <CardDescription>{t('userFriends.description')}</CardDescription>
      </CardHeader>
      <CardContent>
        <Tabs defaultValue="friends">
          <TabsList className="mb-4">
            <TabsTrigger value="friends">
              {t('userFriends.friendsTab')} ({friends.length})
            </TabsTrigger>
            <TabsTrigger value="requests" className="relative">
              {t('userFriends.requestsTab')}
              {incomingCount > 0 && (
                <span className="ml-1.5 rounded-full bg-primary px-1.5 py-0.5 text-[0.65rem] font-bold text-primary-foreground">
                  {incomingCount}
                </span>
              )}
            </TabsTrigger>
          </TabsList>

          <TabsContent value="friends" className="space-y-3">
            {friends.length === 0 ? (
              <p className="text-sm text-muted-foreground">{t('userFriends.noFriends')}</p>
            ) : (
              <div className="grid gap-2">
                {friends.map((friend) => (
                  <div
                    key={friend.id}
                    className="flex items-center justify-between gap-3 rounded-lg border p-3 transition-colors hover:bg-muted/50"
                  >
                    <Link
                      to={localePath(lang, `/u/${encodeURIComponent(friend.username)}`)}
                      className="flex min-w-0 flex-1 items-center gap-3"
                    >
                      <div className="relative">
                        <UserAvatar user={friend} className="size-10 text-sm" />
                        <span
                          className={`absolute -right-0.5 -bottom-0.5 size-2.5 rounded-full ring-2 ring-background ${
                            friend.online ? 'animate-pulse bg-emerald-500' : 'bg-muted-foreground/50'
                          }`}
                        />
                      </div>
                      <div className="min-w-0 flex-1">
                        <div className="flex items-center gap-2">
                          <span className="truncate font-medium">{friend.displayName}</span>
                          <span className="text-xs text-muted-foreground">@{friend.username}</span>
                        </div>
                        <p className="text-xs text-muted-foreground">
                          {friend.online ? (
                            <span className="font-medium text-emerald-600 dark:text-emerald-400">{t('userFriends.online')}</span>
                          ) : (
                            t('userFriends.offline')
                          )}
                        </p>
                      </div>
                    </Link>
                    <div className="flex items-center gap-1">
                      <Button
                        variant="ghost"
                        size="icon"
                        onClick={() => useChatStore.getState().openDirectChat(friend.id, user.id)}
                        className="size-8 text-muted-foreground hover:text-primary"
                        title={t('chat.sendMessage')}
                      >
                        <MessageSquareIcon className="size-4" />
                      </Button>
                      <Button
                        variant="ghost"
                        size="icon"
                        onClick={() => setToRemove(friend)}
                        className="size-8 text-muted-foreground hover:text-destructive"
                        title={t('userFriends.unfriend')}
                      >
                        <UserMinusIcon className="size-4" />
                      </Button>
                    </div>
                  </div>
                ))}
              </div>
            )}
          </TabsContent>

          <TabsContent value="requests" className="space-y-6">
            <div className="space-y-3">
              <h3 className="text-sm font-semibold">
                {t('userFriends.incomingTitle')} ({incoming.length})
              </h3>
              {incoming.length === 0 ? (
                <p className="text-xs text-muted-foreground">{t('userFriends.noIncoming')}</p>
              ) : (
                <div className="grid gap-2">
                  {incoming.map((req) => (
                    <div key={req.id} className="flex flex-wrap items-center justify-between gap-3 rounded-lg border p-3">
                      <Link to={localePath(lang, `/u/${encodeURIComponent(req.user.username)}`)} className="flex items-center gap-3">
                        <UserAvatar user={req.user} className="size-9 text-xs" />
                        <div>
                          <div className="flex items-center gap-2">
                            <span className="text-sm font-medium">{req.user.displayName}</span>
                            <span className="text-xs text-muted-foreground">@{req.user.username}</span>
                          </div>
                          {req.message && <p className="mt-0.5 text-xs text-muted-foreground italic">"{req.message}"</p>}
                        </div>
                      </Link>
                      <div className="flex items-center gap-2">
                        <Button size="sm" disabled={acceptRequest.isPending} onClick={() => onAccept(req.id)} className="h-8 gap-1 text-xs">
                          <CheckIcon className="size-3" />
                          {t('userFriends.accept')}
                        </Button>
                        <Button
                          variant="outline"
                          size="sm"
                          disabled={rejectRequest.isPending}
                          onClick={() => onReject(req.id)}
                          className="h-8 text-xs"
                        >
                          {t('userFriends.reject')}
                        </Button>
                      </div>
                    </div>
                  ))}
                </div>
              )}
            </div>

            <div className="space-y-3">
              <h3 className="text-sm font-semibold">
                {t('userFriends.outgoingTitle')} ({outgoing.length})
              </h3>
              {outgoing.length === 0 ? (
                <p className="text-xs text-muted-foreground">{t('userFriends.noOutgoing')}</p>
              ) : (
                <div className="grid gap-2">
                  {outgoing.map((req) => (
                    <div key={req.id} className="flex flex-wrap items-center justify-between gap-3 rounded-lg border p-3">
                      <Link to={localePath(lang, `/u/${encodeURIComponent(req.user.username)}`)} className="flex items-center gap-3">
                        <UserAvatar user={req.user} className="size-9 text-xs" />
                        <div>
                          <div className="flex items-center gap-2">
                            <span className="text-sm font-medium">{req.user.displayName}</span>
                            <span className="text-xs text-muted-foreground">@{req.user.username}</span>
                          </div>
                          {req.message && <p className="mt-0.5 text-xs text-muted-foreground italic">"{req.message}"</p>}
                        </div>
                      </Link>
                      <Button
                        variant="outline"
                        size="sm"
                        disabled={rejectRequest.isPending}
                        onClick={() => onCancel(req.id)}
                        className="h-8 text-xs text-muted-foreground"
                      >
                        {t('userFriends.cancel')}
                      </Button>
                    </div>
                  ))}
                </div>
              )}
            </div>
          </TabsContent>
        </Tabs>

        <AlertDialog open={!!toRemove} onOpenChange={(open) => !open && setToRemove(null)}>
          <AlertDialogContent>
            <AlertDialogHeader>
              <AlertDialogTitle>{t('userFriends.unfriend')}</AlertDialogTitle>
              <AlertDialogDescription>{t('userFriends.unfriendConfirm', { name: toRemove?.displayName })}</AlertDialogDescription>
            </AlertDialogHeader>
            <AlertDialogFooter>
              <AlertDialogCancel>{t('userFriends.cancel')}</AlertDialogCancel>
              <AlertDialogAction onClick={onUnfriend} className="text-destructive-foreground bg-destructive hover:bg-destructive/90">
                {t('userFriends.unfriend')}
              </AlertDialogAction>
            </AlertDialogFooter>
          </AlertDialogContent>
        </AlertDialog>
      </CardContent>
    </Card>
  )
}

export default function Account() {
  const { t } = useTranslation()
  return (
    <div className="mx-auto grid max-w-xl gap-6">
      <h1 className="text-2xl font-semibold tracking-tight">{t('account.title')}</h1>
      <RequireUser>
        {(user) => (
          <>
            <ProfileCard user={user} />
            <FriendsCard user={user} />
            <SubscriptionCard user={user} />
            <PasswordCard user={user} />
            <IdentitiesCard />
            <TotpCard user={user} />
          </>
        )}
      </RequireUser>
    </div>
  )
}
