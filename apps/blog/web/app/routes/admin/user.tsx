import { zodResolver } from '@hookform/resolvers/zod'
import { ArrowLeftIcon } from 'lucide-react'
import { useState } from 'react'
import { useForm, useWatch } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import { Link, useNavigate, useParams } from 'react-router'
import { toast } from 'sonner'
import { z } from 'zod'
import { BanDialog } from '~/components/admin/ban-dialog'
import { OnlineState, UserBadges } from '~/components/admin/user-state'
import { FormError, FormField } from '~/components/form'
import { LocalDate } from '~/components/local-date'
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
  AlertDialogTrigger,
} from '~/components/ui/alert-dialog'
import { Button } from '~/components/ui/button'
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '~/components/ui/card'
import { Dialog, DialogContent, DialogDescription, DialogHeader, DialogTitle, DialogTrigger } from '~/components/ui/dialog'
import { Input } from '~/components/ui/input'
import { Label } from '~/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '~/components/ui/select'
import { Skeleton } from '~/components/ui/skeleton'
import { Switch } from '~/components/ui/switch'
import { UserAvatar } from '~/components/user-avatar'
import { errorMessage, formatDuration, formatRelative } from '~/i18n'
import { applyFieldErrors } from '~/lib/form-errors'
import { formatBytes } from '~/lib/drive'
import { displayName, email, password, username } from '~/lib/forms'
import { localePath, useLang } from '~/lib/lang'
import { useAdminUser, useDeleteUser, useSetUserDrive, useSetUserWrite, useUnbanUser, useUpdateUser, useUserSecurity } from '~/lib/queries'
import { type AdminUserDetails, deniableActions, Lang, langs } from '~/lib/schemas'
import { describeUserAgent } from '~/lib/user-agent'
import { useNow } from '~/lib/use-now'

const profileForm = z.object({ username, displayName, email, emailVerified: z.boolean(), locale: Lang })
type ProfileForm = z.infer<typeof profileForm>

function Profile({ user }: { user: AdminUserDetails }) {
  const { t } = useTranslation()
  const update = useUpdateUser(user.id)
  const form = useForm<ProfileForm>({
    resolver: zodResolver(profileForm),
    values: {
      username: user.username,
      displayName: user.displayName,
      email: user.email,
      emailVerified: user.emailVerified,
      locale: user.locale,
    },
    // The page refreshes every half minute: what is being typed stays
    resetOptions: { keepDirtyValues: true },
  })
  const errors = form.formState.errors
  const values = useWatch({ control: form.control })
  const onSubmit = form.handleSubmit((input) =>
    update.mutate(input, {
      onSuccess: () => toast.success(t('admin.saved')),
      onError: (error) => applyFieldErrors(form, error),
    }),
  )

  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('admin.profile')}</CardTitle>
      </CardHeader>
      <CardContent>
        <form method="post" onSubmit={onSubmit} className="grid gap-3 sm:grid-cols-2" noValidate>
          <FormField label={t('admin.username')} error={errors.username?.message}>
            {(id) => <Input id={id} autoComplete="off" {...form.register('username')} />}
          </FormField>
          <FormField label={t('admin.displayName')} error={errors.displayName?.message}>
            {(id) => <Input id={id} autoComplete="off" {...form.register('displayName')} />}
          </FormField>
          <FormField label={t('admin.email')} error={errors.email?.message}>
            {(id) => (
              <Input
                id={id}
                type="email"
                autoComplete="off"
                {...form.register('email', {
                  // A new address isn't verified until the admin says so (as the server does)
                  onChange: (event: React.ChangeEvent<HTMLInputElement>) =>
                    form.setValue('emailVerified', event.target.value.trim() === user.email && user.emailVerified),
                })}
              />
            )}
          </FormField>
          <FormField label={t('admin.locale')}>
            {(id) => (
              <Select value={values.locale} onValueChange={(value) => form.setValue('locale', value as Lang, { shouldDirty: true })}>
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
          <div className="flex items-center gap-2">
            <Switch
              id="user-verified"
              checked={values.emailVerified}
              onCheckedChange={(checked) => form.setValue('emailVerified', checked, { shouldDirty: true })}
            />
            <Label htmlFor="user-verified">{t('admin.emailVerified')}</Label>
          </div>
          <div className="flex justify-end sm:col-span-2">
            <Button type="submit" disabled={update.isPending || !form.formState.isDirty}>
              {update.isPending ? t('admin.saving') : t('admin.save')}
            </Button>
          </div>
        </form>
      </CardContent>
    </Card>
  )
}

// Saved as soon as something changes
function RoleAndPermissions({ user }: { user: AdminUserDetails }) {
  const { t, i18n } = useTranslation()
  const update = useUpdateUser(user.id)
  const onError = (error: unknown) => toast.error(errorMessage(i18n, error))

  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('admin.role')}</CardTitle>
        <CardDescription>{t('admin.roleHint')}</CardDescription>
      </CardHeader>
      <CardContent className="grid gap-5">
        <Select
          value={user.role}
          disabled={update.isPending}
          onValueChange={(role) => update.mutate({ role: role as 'user' | 'moderator' }, { onError })}
        >
          <SelectTrigger className="w-40" aria-label={t('admin.role')}>
            <SelectValue />
          </SelectTrigger>
          <SelectContent>
            <SelectItem value="user">{t('admin.roles.user')}</SelectItem>
            <SelectItem value="moderator">{t('admin.roles.moderator')}</SelectItem>
          </SelectContent>
        </Select>
        <div className="grid gap-3">
          <div>
            <h3 className="font-medium">{t('admin.permissions')}</h3>
            <p className="text-sm text-muted-foreground">{t('admin.permissionsHint')}</p>
          </div>
          {deniableActions.map((action) => (
            <div key={action} className="flex items-center gap-2">
              <Switch
                id={`can-${action}`}
                checked={!user.denied.includes(action)}
                disabled={update.isPending}
                onCheckedChange={(allowed) =>
                  update.mutate(
                    { denied: allowed ? user.denied.filter((denied) => denied !== action) : [...user.denied, action] },
                    { onError },
                  )
                }
              />
              <Label htmlFor={`can-${action}`}>{t(`admin.can.${action}`)}</Label>
            </div>
          ))}
        </div>
      </CardContent>
    </Card>
  )
}

const GiB = 1024 ** 3

// Writing: posts of its own, which it edits and invites others to edit (the admin always writes)
function Writing({ user }: { user: AdminUserDetails }) {
  const { t, i18n } = useTranslation()
  const save = useSetUserWrite(user.id)
  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('admin.writing')}</CardTitle>
        <CardDescription>{t('admin.writingHint')}</CardDescription>
      </CardHeader>
      <CardContent className="flex items-center gap-2">
        <Switch
          id="write-enabled"
          checked={user.write}
          disabled={save.isPending}
          onCheckedChange={(enabled) =>
            save.mutate(enabled, {
              onSuccess: () => toast.success(t(enabled ? 'admin.writingOn' : 'admin.writingOff')),
              onError: (error) => toast.error(errorMessage(i18n, error)),
            })
          }
        />
        <Label htmlFor="write-enabled">{t('admin.writingEnabled')}</Label>
      </CardContent>
    </Card>
  )
}

// The account's drive: on or off, its own size and days of trash or the defaults; a link to what is in it
function Drive({ user }: { user: AdminUserDetails }) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const save = useSetUserDrive(user.id)
  const drive = user.drive
  const [size, setSize] = useState(drive?.quota != null ? String(drive.quota / GiB) : '')
  const [days, setDays] = useState(drive?.trashDays != null ? String(drive.trashDays) : '')
  if (!drive) return null

  const quota = size.trim() === '' ? null : Math.round(Number(size) * GiB)
  const trashDays = days.trim() === '' ? null : Number(days)
  const valid =
    (quota === null || (Number.isFinite(quota) && quota >= 0)) &&
    (trashDays === null || (Number.isInteger(trashDays) && trashDays >= 0 && trashDays <= 365))
  const apply = (enabled: boolean, nextQuota: number | null, nextDays: number | null) =>
    save.mutate(
      { enabled, quota: nextQuota, trashDays: nextDays },
      { onSuccess: () => toast.success(t('admin.driveSaved')), onError: (error) => toast.error(errorMessage(i18n, error)) },
    )

  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('admin.drive')}</CardTitle>
        <CardDescription>{user.role === 'admin' ? t('admin.driveOwnHint') : t('admin.driveHint')}</CardDescription>
      </CardHeader>
      <CardContent className="grid gap-4">
        {/* The admin always has one */}
        <div className={user.role === 'admin' ? 'hidden' : 'flex items-center gap-2'}>
          <Switch
            id="drive-enabled"
            checked={drive.enabled}
            disabled={save.isPending}
            onCheckedChange={(enabled) => apply(enabled, drive.quota, drive.trashDays)}
          />
          <Label htmlFor="drive-enabled">{t('admin.driveEnabled')}</Label>
        </div>
        <form
          className="flex flex-wrap items-end gap-2"
          onSubmit={(event) => {
            event.preventDefault()
            if (valid) apply(drive.enabled, quota, trashDays)
          }}
        >
          <div className="grid gap-1.5">
            <Label htmlFor="drive-quota">{t('admin.driveQuota')}</Label>
            <Input
              id="drive-quota"
              inputMode="decimal"
              className="w-40"
              value={size}
              placeholder={t('admin.driveQuotaPlaceholder', { size: formatBytes(drive.defaultQuota, lang) })}
              aria-invalid={valid ? undefined : true}
              onChange={(event) => setSize(event.target.value.replace(/[^\d.]/g, ''))}
            />
          </div>
          <div className="grid gap-1.5">
            <Label htmlFor="drive-trash-days">{t('admin.driveTrashDays')}</Label>
            <Input
              id="drive-trash-days"
              inputMode="numeric"
              className="w-40"
              value={days}
              placeholder={t('admin.driveTrashDaysPlaceholder', { count: drive.defaultTrashDays })}
              aria-invalid={valid ? undefined : true}
              onChange={(event) => setDays(event.target.value.replace(/\D/g, ''))}
            />
          </div>
          <Button
            type="submit"
            variant="outline"
            disabled={!valid || save.isPending || (quota === drive.quota && trashDays === drive.trashDays)}
          >
            {t('drive.save')}
          </Button>
        </form>
        <p className="-mt-2 text-xs text-muted-foreground">{t('admin.driveTrashDaysHint')}</p>
        <div className="flex flex-wrap items-center gap-3">
          <p className="text-sm text-muted-foreground">{t('admin.driveUsed', { size: formatBytes(drive.used, lang) })}</p>
          <Button variant="outline" size="sm" asChild>
            <Link to={localePath(lang, `/admin/users/${user.id}/drive`)}>{t('admin.openDrive')}</Link>
          </Button>
        </div>
      </CardContent>
    </Card>
  )
}

const passwordForm = z.object({ password })

function Security({ user }: { user: AdminUserDetails }) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const update = useUpdateUser(user.id)
  const { mailReset, disableTotp, endSessions, endSession } = useUserSecurity(user.id)
  const now = useNow()
  const form = useForm<z.infer<typeof passwordForm>>({ resolver: zodResolver(passwordForm), defaultValues: { password: '' } })
  const onError = (error: unknown) => toast.error(errorMessage(i18n, error))

  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('admin.security')}</CardTitle>
      </CardHeader>
      <CardContent className="grid gap-6">
        <div className="grid gap-2">
          <p className="flex flex-wrap gap-3 text-sm">
            <span className="font-medium">{t('admin.password')}</span>
            <span className="text-muted-foreground">{user.hasPassword ? t('admin.hasPassword') : t('admin.noPassword')}</span>
          </p>
          <form
            method="post"
            className="flex flex-wrap items-start gap-2"
            noValidate
            onSubmit={form.handleSubmit((input) =>
              update.mutate(input, {
                onSuccess: () => {
                  toast.success(t('admin.passwordSet'))
                  form.reset()
                },
                onError: (error) => applyFieldErrors(form, error) || onError(error),
              }),
            )}
          >
            <div className="min-w-56 flex-1">
              <FormField label={t('admin.newPassword')} hint={t('admin.newPasswordHint')} error={form.formState.errors.password?.message}>
                {(id, describedBy) => (
                  <Input
                    id={id}
                    type="password"
                    autoComplete="new-password"
                    aria-describedby={describedBy}
                    {...form.register('password')}
                  />
                )}
              </FormField>
            </div>
            <Button type="submit" variant="outline" className="mt-6" disabled={update.isPending}>
              {t('admin.setPassword')}
            </Button>
          </form>
          <div>
            <Button
              variant="outline"
              size="sm"
              disabled={mailReset.isPending}
              onClick={() => mailReset.mutate(undefined, { onSuccess: () => toast.success(t('admin.resetMailed')), onError })}
            >
              {t('admin.mailReset')}
            </Button>
          </div>
        </div>

        <div className="flex flex-wrap items-center gap-3 text-sm">
          <span className="font-medium">{t('admin.totp')}</span>
          <span className="text-muted-foreground">{user.totpEnabled ? t('admin.totpOn') : t('admin.totpOff')}</span>
          {user.totpEnabled && (
            <Button
              variant="outline"
              size="sm"
              disabled={disableTotp.isPending}
              onClick={() => disableTotp.mutate(undefined, { onSuccess: () => toast.success(t('admin.totpDisabled')), onError })}
            >
              {t('admin.disableTotp')}
            </Button>
          )}
        </div>

        <div className="grid gap-1 text-sm">
          <span className="font-medium">{t('admin.identities')}</span>
          {user.identities.length === 0 ? (
            <span className="text-muted-foreground">{t('admin.noIdentities')}</span>
          ) : (
            user.identities.map((identity) => (
              <span key={identity.provider} className="text-muted-foreground">
                {identity.provider === 'github' ? 'GitHub' : 'Google'}: {identity.login}
              </span>
            ))
          )}
        </div>

        <div className="grid gap-2">
          <div className="flex flex-wrap items-center gap-2">
            <h3 className="mr-auto text-sm font-medium">{t('admin.sessions')}</h3>
            {user.sessions.length > 0 && (
              <Button
                variant="outline"
                size="sm"
                disabled={endSessions.isPending}
                onClick={() => endSessions.mutate(undefined, { onSuccess: () => toast.success(t('admin.sessionsEnded')), onError })}
              >
                {t('admin.endAllSessions')}
              </Button>
            )}
          </div>
          {user.sessions.length === 0 ? (
            <p className="text-sm text-muted-foreground">{t('admin.noSessions')}</p>
          ) : (
            <ul className="divide-y rounded-lg border text-sm">
              {user.sessions.map((session) => (
                <li key={session.id} className="flex flex-wrap items-center gap-x-3 gap-y-1 p-2">
                  <span className="font-medium" title={session.userAgent}>
                    {describeUserAgent(session.userAgent)}
                  </span>
                  <code className="text-xs">{session.ip}</code>
                  <span className="text-xs text-muted-foreground">
                    {t('admin.loggedInAt', { time: formatRelative(lang, session.createdAt, now) })} ·{' '}
                    {t('admin.lastActive', { time: formatRelative(lang, session.lastSeenAt, now) })}
                  </span>
                  <Button
                    variant="ghost"
                    size="xs"
                    className="ml-auto"
                    disabled={endSession.isPending}
                    onClick={() => endSession.mutate(session.id, { onError })}
                  >
                    {t('admin.endSession')}
                  </Button>
                </li>
              ))}
            </ul>
          )}
        </div>
      </CardContent>
    </Card>
  )
}

function Activity({ user }: { user: AdminUserDetails }) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('admin.activity')}</CardTitle>
      </CardHeader>
      <CardContent className="grid gap-1.5 text-sm">
        <OnlineState user={user} />
        {user.online && <span className="text-muted-foreground">{t('admin.connections', { count: user.online.connections })}</span>}
        <span>{t('admin.totalOnline', { duration: formatDuration(i18n, user.onlineSeconds) })}</span>
        <span>
          {t('admin.registeredAt')} <LocalDate iso={user.createdAt} lang={lang} />
        </span>
        <span>
          {t('admin.commentCount', { count: user.commentCount })} · {t('admin.reactionCount', { count: user.reactionCount })}
        </span>
      </CardContent>
    </Card>
  )
}

function DeleteUser({ user, name }: { user: AdminUserDetails; name: string }) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const navigate = useNavigate()
  const remove = useDeleteUser()
  const [open, setOpen] = useState(false)
  const run = (mode: 'anonymize' | 'purge') =>
    remove.mutate(
      { id: user.id, mode },
      {
        onSuccess: () => {
          toast.success(t('admin.userDeleted'))
          setOpen(false)
          void navigate(localePath(lang, '/admin/users'))
        },
        onError: (error) => toast.error(errorMessage(i18n, error)),
      },
    )

  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogTrigger asChild>
        <Button variant="destructive" size="sm">
          {t('admin.deleteUser')}
        </Button>
      </DialogTrigger>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>{t('admin.deleteUserTitle', { name })}</DialogTitle>
          <DialogDescription className="sr-only">{t('admin.deleteUser')}</DialogDescription>
        </DialogHeader>
        <div className="grid gap-4">
          {!user.deleted && (
            <div className="grid gap-2 rounded-lg border p-3">
              <p className="text-sm text-muted-foreground">{t('admin.anonymizeHint')}</p>
              <Button variant="outline" disabled={remove.isPending} onClick={() => run('anonymize')}>
                {t('admin.anonymize')}
              </Button>
            </div>
          )}
          <div className="grid gap-2 rounded-lg border border-destructive/40 p-3">
            <p className="text-sm text-muted-foreground">{t('admin.purgeHint')}</p>
            <AlertDialog>
              <AlertDialogTrigger asChild>
                <Button variant="destructive" disabled={remove.isPending}>
                  {t('admin.purge')}
                </Button>
              </AlertDialogTrigger>
              <AlertDialogContent>
                <AlertDialogHeader>
                  <AlertDialogTitle>{t('admin.purge')}</AlertDialogTitle>
                  <AlertDialogDescription>{t('admin.purgeHint')}</AlertDialogDescription>
                </AlertDialogHeader>
                <AlertDialogFooter>
                  <AlertDialogCancel>{t('admin.cancel')}</AlertDialogCancel>
                  <AlertDialogAction variant="destructive" onClick={() => run('purge')}>
                    {t('admin.purge')}
                  </AlertDialogAction>
                </AlertDialogFooter>
              </AlertDialogContent>
            </AlertDialog>
          </div>
        </div>
      </DialogContent>
    </Dialog>
  )
}

export default function AdminUser() {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const id = Number(useParams().id)
  const user = useAdminUser(id)
  const unban = useUnbanUser()

  if (user.isPending) return <Skeleton className="h-96 w-full" />
  if (!user.data) return <FormError error={user.error} />

  const data = user.data
  const name = data.deleted ? t('comments.deletedUser') : data.displayName
  const managed = data.role !== 'admin'

  return (
    <div className="grid gap-6">
      <Link
        to={localePath(lang, '/admin/users')}
        className="flex w-fit items-center gap-1 text-sm text-muted-foreground hover:text-foreground"
      >
        <ArrowLeftIcon className="size-4" />
        {t('admin.backToUsers')}
      </Link>

      <div className="flex flex-wrap items-center gap-3">
        <span className="relative">
          <UserAvatar user={{ ...data, displayName: name }} className="size-14 text-xl" />
          {data.online && <span className="absolute right-0.5 bottom-0.5 size-3 rounded-full bg-emerald-500 ring-2 ring-background" />}
        </span>
        <div className="grid gap-1">
          <div className="flex flex-wrap items-center gap-2">
            <h1 className="text-xl font-semibold">{name}</h1>
            {!data.deleted && <span className="text-muted-foreground">@{data.username}</span>}
            <UserBadges user={data} />
          </div>
          <span className="text-sm text-muted-foreground">{data.email}</span>
        </div>
        {managed && (
          <div className="ml-auto flex gap-2">
            {!data.deleted &&
              (data.bannedUntil ? (
                <Button
                  variant="outline"
                  size="sm"
                  onClick={() => unban.mutate(data.id, { onError: (error) => toast.error(errorMessage(i18n, error)) })}
                >
                  {t('admin.unban')}
                </Button>
              ) : (
                <BanDialog userId={data.id} name={name} />
              ))}
            <DeleteUser user={data} name={name} />
          </div>
        )}
      </div>

      {!managed && <p className="text-sm text-muted-foreground">{t('admin.adminAccountHint')}</p>}
      {data.deleted && <p className="text-sm text-muted-foreground">{t('admin.deletedAccountHint')}</p>}

      <div className="grid gap-6 lg:grid-cols-2">
        {managed && !data.deleted && <Profile user={data} />}
        {managed && !data.deleted && <RoleAndPermissions user={data} />}
        {managed && !data.deleted && <Security user={data} />}
        {/* Also the admin's own: its size starts at 0 like everyone's */}
        {!data.deleted && data.role !== 'admin' && <Writing user={data} />}
        {!data.deleted && <Drive user={data} />}
        <Activity user={data} />
      </div>
    </div>
  )
}
