import { CheckIcon, CrownIcon, LogOutIcon, MoreHorizontalIcon, PencilIcon, UserMinusIcon, UserPlusIcon, XIcon } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Link } from 'react-router'
import { toast } from 'sonner'
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
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '~/components/ui/dialog'
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from '~/components/ui/dropdown-menu'
import { Input } from '~/components/ui/input'
import { Skeleton } from '~/components/ui/skeleton'
import { UserAvatar } from '~/components/user-avatar'
import { errorMessage } from '~/i18n'
import { localePath, useLang } from '~/lib/lang'
import {
  useAddChatGroupMembers,
  useChatGroupMembers,
  useLeaveChatGroup,
  useRemoveChatGroupMember,
  useRenameChatGroup,
  useTransferChatGroup,
  useUserFriends,
} from '~/lib/queries'
import type { ChatChannelItem, ChatGroupMemberItem } from '~/lib/schemas'
import { chatError, groupIdOf } from './chat-utils'

function OnlineDot({ online }: { online: boolean }) {
  return (
    <span
      className={`absolute -right-0.5 -bottom-0.5 size-2 rounded-full ring-1 ring-background ${
        online ? 'animate-pulse bg-emerald-500' : 'bg-muted-foreground/50'
      }`}
    />
  )
}

// The owner's friends not in the group yet, to invite
function AddMembersDialog({
  groupId,
  memberIds,
  open,
  onOpenChange,
}: {
  groupId: number
  memberIds: number[]
  open: boolean
  onOpenChange: (open: boolean) => void
}) {
  const { t, i18n } = useTranslation()
  const friendsQuery = useUserFriends(open)
  const addMutation = useAddChatGroupMembers()
  const [selected, setSelected] = useState<number[]>([])
  const candidates = (friendsQuery.data?.friends ?? []).filter((f) => !memberIds.includes(f.id))

  const onSubmit = () =>
    addMutation.mutate(
      { groupId, memberIds: selected },
      {
        onSuccess: () => {
          toast.success(t('chat.membersAdded'))
          setSelected([])
          onOpenChange(false)
        },
        onError: (error) => toast.error(chatError(i18n, error)),
      },
    )

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-xs">
        <DialogHeader>
          <DialogTitle>{t('chat.addMembers')}</DialogTitle>
          <DialogDescription>{t('chat.selectFriends')}</DialogDescription>
        </DialogHeader>
        <div className="max-h-60 space-y-1 overflow-y-auto rounded-md border p-2">
          {candidates.length === 0 ? (
            <p className="p-2 text-xs text-muted-foreground">{t('chat.noFriendsToAdd')}</p>
          ) : (
            candidates.map((f) => (
              <label key={f.id} className="flex cursor-pointer items-center gap-2.5 rounded-md p-1.5 hover:bg-muted/50">
                <input
                  type="checkbox"
                  checked={selected.includes(f.id)}
                  onChange={() => setSelected((prev) => (prev.includes(f.id) ? prev.filter((i) => i !== f.id) : [...prev, f.id]))}
                  className="size-4"
                />
                <UserAvatar user={f} className="size-6 text-[0.65rem]" />
                <span className="truncate text-xs font-medium">{f.displayName}</span>
              </label>
            ))
          )}
        </div>
        <DialogFooter>
          <Button variant="outline" size="sm" onClick={() => onOpenChange(false)}>
            {t('userFriends.cancel')}
          </Button>
          <Button size="sm" onClick={onSubmit} disabled={selected.length === 0 || addMutation.isPending}>
            {t('chat.addMembers')}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}

type MemberConfirm = { kind: 'remove' | 'transfer'; member: ChatGroupMemberItem } | { kind: 'leave' }

// A group's members and, for its owner, its management: its name, who is in it, who owns it. Anyone
// may leave; the owner leaving dissolves it
export function GroupPanel({ channel, currentUserId, onLeft }: { channel: ChatChannelItem; currentUserId: number; onLeft: () => void }) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const groupId = groupIdOf(channel.id)
  const membersQuery = useChatGroupMembers(groupId)
  const renameMutation = useRenameChatGroup()
  const removeMutation = useRemoveChatGroupMember()
  const transferMutation = useTransferChatGroup()
  const leaveMutation = useLeaveChatGroup()
  const [renaming, setRenaming] = useState<string | null>(null)
  const [adding, setAdding] = useState(false)
  const [confirm, setConfirm] = useState<MemberConfirm | null>(null)

  const members = membersQuery.data?.members ?? []
  const isOwner = channel.ownerId === currentUserId || members.some((m) => m.id === currentUserId && m.role === 'owner')

  const onRename = () => {
    const name = renaming?.trim()
    if (!name) return
    renameMutation.mutate(
      { groupId, name },
      {
        onSuccess: () => {
          toast.success(t('chat.renamed'))
          setRenaming(null)
        },
        onError: (error) => toast.error(chatError(i18n, error)),
      },
    )
  }

  const onConfirm = () => {
    if (!confirm) return
    const onError = (error: unknown) => toast.error(errorMessage(i18n, error))
    if (confirm.kind === 'remove')
      removeMutation.mutate({ groupId, userId: confirm.member.id }, { onSuccess: () => toast.success(t('chat.memberRemoved')), onError })
    else if (confirm.kind === 'transfer')
      transferMutation.mutate(
        { groupId, userId: confirm.member.id },
        { onSuccess: () => toast.success(t('chat.ownerTransferred')), onError },
      )
    else
      leaveMutation.mutate(groupId, {
        onSuccess: () => {
          toast.success(t(isOwner ? 'chat.groupDissolved' : 'chat.leftGroup'))
          onLeft()
        },
        onError,
      })
    setConfirm(null)
  }

  const confirmTitle =
    confirm?.kind === 'remove'
      ? t('chat.removeMember')
      : confirm?.kind === 'transfer'
        ? t('chat.transferOwner')
        : t(isOwner ? 'chat.dissolveGroup' : 'chat.leaveGroup')
  const confirmText =
    confirm?.kind === 'remove'
      ? t('chat.removeMemberConfirm', { name: confirm.member.displayName })
      : confirm?.kind === 'transfer'
        ? t('chat.transferOwnerConfirm', { name: confirm.member.displayName })
        : t(isOwner ? 'chat.dissolveGroupConfirm' : 'chat.leaveGroupConfirm')

  return (
    <div className="flex h-full flex-col">
      {/* Its name, changed in place by its owner */}
      <div className="border-b px-3 py-3">
        {renaming !== null ? (
          <div className="flex items-center gap-1">
            <Input
              value={renaming}
              onChange={(e) => setRenaming(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter') onRename()
                if (e.key === 'Escape') setRenaming(null)
              }}
              maxLength={50}
              autoFocus
              aria-label={t('chat.groupName')}
              className="h-8 text-sm"
            />
            <Button
              size="icon"
              className="size-8 shrink-0"
              onClick={onRename}
              aria-label={t('chat.save')}
              disabled={renameMutation.isPending}
            >
              <CheckIcon className="size-4" />
            </Button>
            <Button
              variant="ghost"
              size="icon"
              className="size-8 shrink-0"
              onClick={() => setRenaming(null)}
              aria-label={t('userFriends.cancel')}
            >
              <XIcon className="size-4" />
            </Button>
          </div>
        ) : (
          <div className="flex items-center justify-between gap-2">
            <div className="min-w-0">
              <p className="truncate text-sm font-semibold">{channel.name}</p>
              <p className="text-xs text-muted-foreground">{t('chat.groupMembers', { count: members.length || channel.memberCount })}</p>
            </div>
            {isOwner && (
              <Button
                variant="ghost"
                size="icon"
                className="size-7"
                title={t('chat.rename')}
                aria-label={t('chat.rename')}
                onClick={() => setRenaming(channel.name)}
              >
                <PencilIcon className="size-3.5" />
              </Button>
            )}
          </div>
        )}
      </div>

      <div className="flex items-center justify-between px-3 pt-3 pb-1">
        <span className="text-xs font-semibold tracking-wider text-muted-foreground uppercase">{t('chat.members')}</span>
        {isOwner && (
          <Button variant="ghost" size="sm" className="h-7 gap-1 text-xs" onClick={() => setAdding(true)}>
            <UserPlusIcon className="size-3.5" />
            {t('chat.addMembers')}
          </Button>
        )}
      </div>

      <div className="min-h-0 flex-1 space-y-0.5 overflow-y-auto px-2 pb-2">
        {membersQuery.isPending ? (
          <Skeleton className="h-24 w-full" />
        ) : (
          members.map((m) => (
            <div key={m.id} className="flex items-center justify-between gap-2 rounded-md p-1.5 hover:bg-muted/40">
              <Link
                to={localePath(lang, `/u/${encodeURIComponent(m.username)}`)}
                className="flex min-w-0 items-center gap-2 hover:underline"
              >
                <span className="relative shrink-0">
                  <UserAvatar user={m} className="size-7 text-[0.65rem]" />
                  <OnlineDot online={m.online} />
                </span>
                <span className="truncate text-xs font-medium">
                  {m.displayName}
                  {m.id === currentUserId && <span className="text-muted-foreground"> ({t('chat.you')})</span>}
                </span>
              </Link>
              <div className="flex shrink-0 items-center gap-1">
                {m.role === 'owner' && (
                  <span className="flex items-center gap-0.5 rounded-full bg-primary/10 px-2 py-0.5 text-[0.65rem] font-medium text-primary">
                    <CrownIcon className="size-3" />
                    {t('chat.ownerBadge')}
                  </span>
                )}
                {isOwner && m.id !== currentUserId && (
                  <DropdownMenu>
                    <DropdownMenuTrigger asChild>
                      <Button variant="ghost" size="icon" className="size-6 text-muted-foreground" aria-label={t('chat.memberActions')}>
                        <MoreHorizontalIcon className="size-3.5" />
                      </Button>
                    </DropdownMenuTrigger>
                    <DropdownMenuContent align="end" className="w-36">
                      <DropdownMenuItem onSelect={() => setConfirm({ kind: 'transfer', member: m })}>
                        <CrownIcon className="mr-2 size-4" />
                        {t('chat.transferOwner')}
                      </DropdownMenuItem>
                      <DropdownMenuItem onSelect={() => setConfirm({ kind: 'remove', member: m })} className="text-destructive">
                        <UserMinusIcon className="mr-2 size-4" />
                        {t('chat.removeMember')}
                      </DropdownMenuItem>
                    </DropdownMenuContent>
                  </DropdownMenu>
                )}
              </div>
            </div>
          ))
        )}
      </div>

      <div className="border-t p-2">
        <Button
          variant="outline"
          size="sm"
          onClick={() => setConfirm({ kind: 'leave' })}
          className="w-full text-xs text-destructive hover:bg-destructive/10"
        >
          <LogOutIcon className="mr-1.5 size-3.5" />
          {t(isOwner ? 'chat.dissolveGroup' : 'chat.leaveGroup')}
        </Button>
      </div>

      <AddMembersDialog groupId={groupId} memberIds={members.map((m) => m.id)} open={adding} onOpenChange={setAdding} />

      <AlertDialog open={confirm !== null} onOpenChange={(open) => !open && setConfirm(null)}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>{confirmTitle}</AlertDialogTitle>
            <AlertDialogDescription>{confirmText}</AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>{t('userFriends.cancel')}</AlertDialogCancel>
            <AlertDialogAction
              onClick={onConfirm}
              className={confirm?.kind === 'transfer' ? '' : 'text-destructive-foreground bg-destructive hover:bg-destructive/90'}
            >
              {confirmTitle}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </div>
  )
}
