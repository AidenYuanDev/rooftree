import { zodResolver } from '@hookform/resolvers/zod'
import { PlusIcon, Trash2Icon } from 'lucide-react'
import { useState } from 'react'
import { useForm, useWatch } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import { Link, useNavigate, useSearchParams } from 'react-router'
import { toast } from 'sonner'
import { z } from 'zod'
import { BanDialog } from '~/components/admin/ban-dialog'
import { OnlineState, UserBadges } from '~/components/admin/user-state'
import { FormError, FormField } from '~/components/form'
import { LocalDate } from '~/components/local-date'
import { Button } from '~/components/ui/button'
import { Dialog, DialogContent, DialogFooter, DialogHeader, DialogTitle, DialogTrigger } from '~/components/ui/dialog'
import { Input } from '~/components/ui/input'
import { Label } from '~/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '~/components/ui/select'
import { Skeleton } from '~/components/ui/skeleton'
import { Switch } from '~/components/ui/switch'
import { UserAvatar } from '~/components/user-avatar'
import { errorMessage, formatDuration } from '~/i18n'
import { applyFieldErrors } from '~/lib/form-errors'
import { displayName, email, password, username } from '~/lib/forms'
import { localePath, useLang } from '~/lib/lang'
import {
  type UserFilters,
  useAddIpBan,
  useAdminUsers,
  useCreateUser,
  useDeleteIpBan,
  useIpBans,
  useMe,
  useOnlineCounts,
  useUnbanUser,
} from '~/lib/queries'
import { type AdminUser, Lang, langs, roles } from '~/lib/schemas'

const statuses = ['online', 'banned', 'deleted'] as const

function IpBans() {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const bans = useIpBans()
  const add = useAddIpBan()
  const remove = useDeleteIpBan()
  const [ip, setIp] = useState('')
  const [reason, setReason] = useState('')
  const [days, setDays] = useState('')

  return (
    <section className="grid gap-3">
      <h2 className="font-semibold">{t('admin.ipBans')}</h2>
      <form
        method="post"
        className="flex flex-wrap gap-2"
        onSubmit={(event) => {
          event.preventDefault()
          add.mutate(
            { ip, reason, days: days ? Number(days) : null },
            {
              onSuccess: () => {
                setIp('')
                setReason('')
                setDays('')
              },
              onError: (error) => toast.error(errorMessage(i18n, error)),
            },
          )
        }}
      >
        <Input
          className="w-44 font-mono"
          placeholder={t('admin.ipAddress')}
          aria-label={t('admin.ipAddress')}
          value={ip}
          onChange={(e) => setIp(e.target.value)}
        />
        <Input
          className="w-24"
          placeholder={t('admin.banDays')}
          aria-label={t('admin.banDays')}
          inputMode="numeric"
          value={days}
          onChange={(e) => setDays(e.target.value.replace(/\D/g, ''))}
        />
        <Input
          className="min-w-40 flex-1"
          placeholder={t('admin.banReason')}
          aria-label={t('admin.banReason')}
          value={reason}
          onChange={(e) => setReason(e.target.value)}
        />
        <Button type="submit" variant="destructive" disabled={add.isPending || !ip.trim()}>
          {t('admin.addIpBan')}
        </Button>
      </form>
      <FormError error={bans.error} />
      {bans.data?.length === 0 && <p className="text-sm text-muted-foreground">{t('admin.noIpBans')}</p>}
      {!!bans.data?.length && (
        <ul className="divide-y rounded-lg border text-sm">
          {bans.data.map((ban) => (
            <li key={ban.ip} className="flex flex-wrap items-center gap-3 p-2">
              <code>{ban.ip}</code>
              <span className="text-muted-foreground">{ban.reason}</span>
              <span className="ml-auto text-xs text-muted-foreground">
                {t('admin.until')}: {ban.expiresAt ? <LocalDate iso={ban.expiresAt} lang={lang} /> : t('admin.forever')}
              </span>
              <Button variant="ghost" size="icon-sm" aria-label={t('admin.unban')} onClick={() => remove.mutate(ban.ip)}>
                <Trash2Icon />
              </Button>
            </li>
          ))}
        </ul>
      )}
    </section>
  )
}

const createForm = z.object({
  username,
  displayName,
  email,
  password: z.union([z.literal(''), password]),
  role: z.enum(['user', 'moderator']),
  locale: Lang,
  emailVerified: z.boolean(),
})
type CreateForm = z.infer<typeof createForm>

function CreateUserDialog() {
  const { t } = useTranslation()
  const lang = useLang()
  const navigate = useNavigate()
  const create = useCreateUser()
  const [open, setOpen] = useState(false)
  const form = useForm<CreateForm>({
    resolver: zodResolver(createForm),
    defaultValues: { username: '', displayName: '', email: '', password: '', role: 'user', locale: lang, emailVerified: true },
  })
  const errors = form.formState.errors
  const values = useWatch({ control: form.control })
  const onSubmit = form.handleSubmit(({ password: chosen, ...input }) =>
    create.mutate(chosen ? { ...input, password: chosen } : input, {
      onSuccess: ({ id }) => {
        toast.success(t('admin.userCreated'))
        setOpen(false)
        form.reset()
        void navigate(localePath(lang, `/admin/users/${id}`))
      },
      onError: (error) => applyFieldErrors(form, error),
    }),
  )

  return (
    <Dialog open={open} onOpenChange={setOpen}>
      <DialogTrigger asChild>
        <Button size="sm">
          <PlusIcon />
          {t('admin.newUser')}
        </Button>
      </DialogTrigger>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>{t('admin.newUser')}</DialogTitle>
        </DialogHeader>
        <form method="post" id="create-user" onSubmit={onSubmit} className="grid gap-3" noValidate>
          <FormField label={t('admin.username')} error={errors.username?.message}>
            {(id) => <Input id={id} autoComplete="off" {...form.register('username')} />}
          </FormField>
          <FormField label={t('admin.displayName')} error={errors.displayName?.message}>
            {(id) => <Input id={id} autoComplete="off" {...form.register('displayName')} />}
          </FormField>
          <FormField label={t('admin.email')} error={errors.email?.message}>
            {(id) => <Input id={id} type="email" autoComplete="off" {...form.register('email')} />}
          </FormField>
          <FormField label={t('admin.passwordOptional')} hint={t('admin.passwordOptionalHint')} error={errors.password?.message}>
            {(id, describedBy) => (
              <Input id={id} type="password" autoComplete="new-password" aria-describedby={describedBy} {...form.register('password')} />
            )}
          </FormField>
          <div className="grid grid-cols-2 gap-3">
            <FormField label={t('admin.role')}>
              {(id) => (
                <Select value={values.role} onValueChange={(value) => form.setValue('role', value as CreateForm['role'])}>
                  <SelectTrigger id={id} className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="user">{t('admin.roles.user')}</SelectItem>
                    <SelectItem value="moderator">{t('admin.roles.moderator')}</SelectItem>
                  </SelectContent>
                </Select>
              )}
            </FormField>
            <FormField label={t('admin.locale')}>
              {(id) => (
                <Select value={values.locale} onValueChange={(value) => form.setValue('locale', value as Lang)}>
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
          </div>
          <div className="flex items-center gap-2">
            <Switch
              id="create-user-verified"
              checked={values.emailVerified}
              onCheckedChange={(checked) => form.setValue('emailVerified', checked)}
            />
            <Label htmlFor="create-user-verified">{t('admin.emailVerified')}</Label>
          </div>
        </form>
        <DialogFooter>
          <Button variant="ghost" onClick={() => setOpen(false)}>
            {t('admin.cancel')}
          </Button>
          <Button type="submit" form="create-user" disabled={create.isPending}>
            {t('admin.newUser')}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}

function UserItem({ user, admin }: { user: AdminUser; admin: boolean }) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const unban = useUnbanUser()
  // Moderators ban users, the admin bans moderators too; nobody bans the admin
  const bannable = !user.deleted && user.role !== 'admin' && (admin || user.role === 'user')
  const name = user.deleted ? t('comments.deletedUser') : user.displayName

  return (
    <li className="flex flex-wrap items-center gap-x-3 gap-y-1 p-3 text-sm">
      <span className="relative">
        <UserAvatar user={{ ...user, displayName: name }} className="size-9 text-sm" />
        {user.online && (
          <span
            className="absolute right-0 bottom-0 size-2.5 rounded-full bg-emerald-500 ring-2 ring-background"
            title={t('admin.userStatus.online')}
          />
        )}
      </span>
      <div className="grid min-w-0 flex-1 gap-0.5">
        <div className="flex flex-wrap items-center gap-x-2 gap-y-1">
          {admin ? (
            <Link to={localePath(lang, `/admin/users/${user.id}`)} className="font-medium hover:underline">
              {name}
            </Link>
          ) : (
            <span className="font-medium">{name}</span>
          )}
          {!user.deleted && <span className="text-muted-foreground">@{user.username}</span>}
          {user.email && !user.deleted && <span className="text-muted-foreground">{user.email}</span>}
          <UserBadges user={user} />
        </div>
        <div className="flex flex-wrap gap-x-3 text-xs text-muted-foreground">
          <OnlineState user={user} />
          {user.onlineSeconds > 0 && <span>{t('admin.totalOnline', { duration: formatDuration(i18n, user.onlineSeconds) })}</span>}
          <span>{t('admin.commentCount', { count: user.commentCount })}</span>
          <span>
            {t('admin.registeredAt')} <LocalDate iso={user.createdAt} lang={lang} />
          </span>
        </div>
      </div>
      {bannable &&
        (user.bannedUntil ? (
          <Button
            variant="ghost"
            size="xs"
            onClick={() => unban.mutate(user.id, { onError: (error) => toast.error(errorMessage(i18n, error)) })}
          >
            {t('admin.unban')}
          </Button>
        ) : (
          <BanDialog userId={user.id} name={name} />
        ))}
    </li>
  )
}

export default function AdminUsers() {
  const { t } = useTranslation()
  const me = useMe()
  const admin = me.data?.user?.role === 'admin'
  const [searchParams, setSearchParams] = useSearchParams()
  const filters: UserFilters = {
    q: searchParams.get('q') ?? '',
    role: (roles as readonly string[]).includes(searchParams.get('role') ?? '') ? (searchParams.get('role') as UserFilters['role']) : '',
    status: (statuses as readonly string[]).includes(searchParams.get('status') ?? '')
      ? (searchParams.get('status') as UserFilters['status'])
      : '',
    sort: searchParams.get('sort') === 'lastSeen' ? 'lastSeen' : '',
    page: Math.max(1, Number(searchParams.get('page') ?? 1) || 1),
  }
  const users = useAdminUsers(filters)
  const online = useOnlineCounts()
  // A change of filter starts from the first page
  const setFilter = (key: keyof UserFilters, value: string | number) => {
    const next = new URLSearchParams(searchParams)
    if (key !== 'page') next.delete('page')
    if (value === '' || value === 'all' || value === 1) next.delete(key)
    else next.set(key, String(value))
    setSearchParams(next, { replace: key === 'q' })
  }

  return (
    <div className="grid gap-8">
      <section className="grid gap-3">
        <div className="flex flex-wrap items-center gap-2">
          {online.data && (
            <p className="mr-auto flex items-center text-sm text-muted-foreground">
              <span className="mr-2 inline-block size-2 animate-pulse rounded-full bg-emerald-500" />
              {t('admin.onlineSummary', { count: online.data.users, viewers: online.data.viewers })}
            </p>
          )}
          {admin && <CreateUserDialog />}
        </div>
        <div className="flex flex-wrap gap-2">
          <Input
            type="search"
            className="min-w-48 flex-1"
            defaultValue={filters.q}
            placeholder={t('admin.searchUsers')}
            aria-label={t('admin.searchUsers')}
            onChange={(event) => setFilter('q', event.target.value)}
          />
          <Select value={filters.role || 'all'} onValueChange={(value) => setFilter('role', value)}>
            <SelectTrigger className="w-32" aria-label={t('admin.role')}>
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="all">{t('admin.allRoles')}</SelectItem>
              {roles.map((role) => (
                <SelectItem key={role} value={role}>
                  {t(`admin.roles.${role}`)}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          <Select value={filters.status || 'all'} onValueChange={(value) => setFilter('status', value)}>
            <SelectTrigger className="w-32" aria-label={t('admin.status')}>
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="all">{t('admin.userStatus.all')}</SelectItem>
              {statuses.map((status) => (
                <SelectItem key={status} value={status}>
                  {t(`admin.userStatus.${status}`)}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          <Select value={filters.sort || 'newest'} onValueChange={(value) => setFilter('sort', value === 'newest' ? '' : value)}>
            <SelectTrigger className="w-32" aria-label={t('admin.sort')}>
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="newest">{t('admin.sortNewest')}</SelectItem>
              <SelectItem value="lastSeen">{t('admin.sortLastSeen')}</SelectItem>
            </SelectContent>
          </Select>
        </div>
        <FormError error={users.error} />
        {users.isPending && <Skeleton className="h-40 w-full" />}
        {users.data?.users.length === 0 && <p className="py-8 text-center text-muted-foreground">{t('admin.noUsers')}</p>}
        {!!users.data?.users.length && (
          <ul className="divide-y rounded-lg border">
            {users.data.users.map((user) => (
              <UserItem key={user.id} user={user} admin={admin} />
            ))}
          </ul>
        )}
        {users.data && (filters.page > 1 || users.data.hasMore) && (
          <div className="flex justify-center gap-2">
            <Button variant="outline" size="sm" disabled={filters.page <= 1} onClick={() => setFilter('page', filters.page - 1)}>
              ‹
            </Button>
            <span className="px-2 py-1 text-sm">{filters.page}</span>
            <Button variant="outline" size="sm" disabled={!users.data.hasMore} onClick={() => setFilter('page', filters.page + 1)}>
              ›
            </Button>
          </div>
        )}
      </section>
      {admin && <IpBans />}
    </div>
  )
}
