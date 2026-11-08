import { zodResolver } from '@hookform/resolvers/zod'
import { CheckIcon, PencilIcon, XIcon } from 'lucide-react'
import { useState } from 'react'
import { useForm } from 'react-hook-form'
import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import { z } from 'zod'
import { DeleteButton } from '~/components/admin/delete-button'
import { FormError, FormField } from '~/components/form'
import { LocalDate } from '~/components/local-date'
import { Badge } from '~/components/ui/badge'
import { Button } from '~/components/ui/button'
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '~/components/ui/dialog'
import { Input } from '~/components/ui/input'
import { Skeleton } from '~/components/ui/skeleton'
import { Tabs, TabsList, TabsTrigger } from '~/components/ui/tabs'
import { errorMessage } from '~/i18n'
import { applyFieldErrors } from '~/lib/form-errors'
import { useHydrated } from '~/lib/hydrated'
import { useLang } from '~/lib/lang'
import { useAdminFriends, useDeleteFriend, useReviewFriend, useSaveFriend } from '~/lib/queries'
import type { AdminFriend } from '~/lib/schemas'

const friendForm = z.object({
  name: z.string().trim().min(1, 'required').max(50, 'too_long'),
  url: z
    .string()
    .trim()
    .max(255, 'too_long')
    .refine((v) => /^https?:\/\/[^\s@]{4,}$/.test(v), 'invalid'),
  avatar: z
    .string()
    .trim()
    .max(255, 'too_long')
    .refine((v) => v === '' || v.startsWith('https://') || v.startsWith('/uploads/'), 'invalid'),
  description: z.string().trim().max(200, 'too_long'),
  sort: z.number('invalid').int('invalid'),
})
type FriendForm = z.infer<typeof friendForm>

function FriendFields({ id, friend, onDone }: { id: number | null; friend?: AdminFriend; onDone?: () => void }) {
  const { t } = useTranslation()
  const hydrated = useHydrated()
  const save = useSaveFriend()
  const form = useForm<FriendForm>({
    resolver: zodResolver(friendForm),
    defaultValues: friend
      ? { name: friend.name, url: friend.url, avatar: friend.avatar, description: friend.description, sort: friend.sort }
      : { name: '', url: '', avatar: '', description: '', sort: 0 },
  })
  const errors = form.formState.errors
  const onSubmit = form.handleSubmit((input) =>
    save.mutate(
      { id, input },
      {
        onSuccess: () => {
          toast.success(t('admin.saved'))
          if (id === null) form.reset()
          onDone?.()
        },
        onError: (error) => applyFieldErrors(form, error),
      },
    ),
  )

  return (
    <form
      method="post"
      onSubmit={onSubmit}
      className="grid gap-3 rounded-lg border p-4 md:grid-cols-[1fr_1.4fr_1.4fr_1.6fr_5rem_auto] md:items-start"
      noValidate
    >
      <FormField label={t('friends.name')} error={errors.name?.message}>
        {(fieldId) => <Input id={fieldId} {...form.register('name')} />}
      </FormField>
      <FormField label={t('friends.url')} error={errors.url?.message}>
        {(fieldId) => <Input id={fieldId} type="url" {...form.register('url')} />}
      </FormField>
      <FormField label={t('friends.avatar')} error={errors.avatar?.message}>
        {(fieldId) => <Input id={fieldId} {...form.register('avatar')} />}
      </FormField>
      <FormField label={t('friends.descriptionField')} error={errors.description?.message}>
        {(fieldId) => <Input id={fieldId} {...form.register('description')} />}
      </FormField>
      <FormField label={t('admin.sort')} error={errors.sort?.message}>
        {(fieldId) => <Input id={fieldId} type="number" {...form.register('sort', { valueAsNumber: true })} />}
      </FormField>
      <div className="flex gap-2 md:mt-6">
        <Button type="submit" disabled={!hydrated || save.isPending}>
          {id === null ? t('admin.add') : t('admin.save')}
        </Button>
        {onDone && (
          <Button type="button" variant="ghost" onClick={onDone}>
            {t('admin.cancel')}
          </Button>
        )}
      </div>
      <div className="md:col-span-6">
        <FormError error={save.error} />
      </div>
    </form>
  )
}

function RejectDialog({ friend, onClose }: { friend: AdminFriend; onClose: () => void }) {
  const { t, i18n } = useTranslation()
  const review = useReviewFriend()
  const [reason, setReason] = useState('')
  return (
    <Dialog open onOpenChange={(open) => !open && onClose()}>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>{t('admin.rejectFriend', { name: friend.name })}</DialogTitle>
          <DialogDescription>{t('admin.rejectFriendHint')}</DialogDescription>
        </DialogHeader>
        <Input value={reason} maxLength={255} onChange={(event) => setReason(event.target.value)} aria-label={t('admin.rejectReason')} />
        <DialogFooter>
          <Button variant="ghost" onClick={onClose}>
            {t('admin.cancel')}
          </Button>
          <Button
            variant="destructive"
            disabled={review.isPending}
            onClick={() =>
              review.mutate(
                { id: friend.id, status: 'rejected', rejectReason: reason },
                { onSuccess: onClose, onError: (error) => toast.error(errorMessage(i18n, error)) },
              )
            }
          >
            {t('admin.reject')}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}

function Row({ friend }: { friend: AdminFriend }) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const review = useReviewFriend()
  const remove = useDeleteFriend()
  const [editing, setEditing] = useState(false)
  const [rejecting, setRejecting] = useState(false)

  if (editing) return <FriendFields id={friend.id} friend={friend} onDone={() => setEditing(false)} />

  const statusVariant = friend.status === 'approved' ? 'default' : friend.status === 'pending' ? 'secondary' : 'outline'
  return (
    <div className="flex flex-wrap items-start gap-3 p-3">
      {friend.avatar ? (
        <img src={friend.avatar} alt="" className="size-10 rounded-full border object-cover" />
      ) : (
        <span className="flex size-10 items-center justify-center rounded-full bg-muted font-bold">{[...friend.name][0]}</span>
      )}
      <div className="min-w-0 flex-1">
        <div className="flex flex-wrap items-center gap-2">
          <a href={friend.url} target="_blank" rel="noopener" className="font-medium hover:underline">
            {friend.name}
          </a>
          <Badge variant={statusVariant}>{t(`admin.friendStatus.${friend.status}`)}</Badge>
          <span className="truncate font-mono text-xs text-muted-foreground">{friend.url}</span>
        </div>
        {friend.description && <p className="text-sm text-muted-foreground">{friend.description}</p>}
        {friend.message && <p className="mt-1 rounded bg-muted px-2 py-1 text-sm">「{friend.message}」</p>}
        {friend.rejectReason && <p className="mt-1 text-xs text-destructive">{friend.rejectReason}</p>}
        <div className="mt-1 flex gap-3 text-xs text-muted-foreground">
          <LocalDate iso={friend.createdAt} lang={lang} />
          {friend.applicant && (
            <span>{t('admin.appliedBy', { name: `${friend.applicant.displayName} (@${friend.applicant.username})` })}</span>
          )}
          <span>
            {t('admin.sort')} {friend.sort}
          </span>
        </div>
      </div>
      {friend.status === 'pending' && (
        <>
          <Button
            size="sm"
            disabled={review.isPending}
            onClick={() =>
              review.mutate({ id: friend.id, status: 'approved' }, { onError: (error) => toast.error(errorMessage(i18n, error)) })
            }
          >
            <CheckIcon />
            {t('admin.approve')}
          </Button>
          <Button size="sm" variant="outline" onClick={() => setRejecting(true)}>
            <XIcon />
            {t('admin.reject')}
          </Button>
        </>
      )}
      <Button variant="ghost" size="icon-sm" aria-label={t('admin.edit')} onClick={() => setEditing(true)}>
        <PencilIcon />
      </Button>
      <DeleteButton
        title={friend.name}
        onConfirm={() => remove.mutate(friend.id, { onError: (error) => toast.error(errorMessage(i18n, error)) })}
      />
      {rejecting && <RejectDialog friend={friend} onClose={() => setRejecting(false)} />}
    </div>
  )
}

export default function AdminFriends() {
  const { t } = useTranslation()
  const [status, setStatus] = useState('')
  const friends = useAdminFriends(status)

  return (
    <div className="grid gap-6">
      <FriendFields id={null} />
      <Tabs value={status || 'all'} onValueChange={(value) => setStatus(value === 'all' ? '' : value)}>
        <TabsList>
          <TabsTrigger value="all">{t('admin.all')}</TabsTrigger>
          <TabsTrigger value="pending">{t('admin.friendStatus.pending')}</TabsTrigger>
          <TabsTrigger value="approved">{t('admin.friendStatus.approved')}</TabsTrigger>
          <TabsTrigger value="rejected">{t('admin.friendStatus.rejected')}</TabsTrigger>
        </TabsList>
      </Tabs>
      <FormError error={friends.error} />
      {friends.isPending && <Skeleton className="h-40 w-full" />}
      {friends.data?.length === 0 && <p className="py-12 text-center text-muted-foreground">{t('friends.empty')}</p>}
      {friends.data && friends.data.length > 0 && (
        <div className="divide-y rounded-lg border">
          {friends.data.map((friend) => (
            <Row key={friend.id} friend={friend} />
          ))}
        </div>
      )}
    </div>
  )
}
