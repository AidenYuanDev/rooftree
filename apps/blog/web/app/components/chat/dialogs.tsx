import { ArrowLeftIcon, CheckIcon } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import { Button } from '~/components/ui/button'
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '~/components/ui/dialog'
import { Input } from '~/components/ui/input'
import { UserAvatar } from '~/components/user-avatar'
import { errorMessage } from '~/i18n'
import {
  useAcceptFriendRequest,
  useCreateChatGroup,
  useRejectFriendRequest,
  useSendFriendRequest,
  useUserFriendRequests,
  useUserFriends,
} from '~/lib/queries'
import { chatError } from './chat-utils'

// The chat's dialogs, the dock's and the chat page's: friend requests, a friend added, a group made

export function FriendRequestsView({ onBack }: { onBack: () => void }) {
  const { t } = useTranslation()
  const requestsQuery = useUserFriendRequests(true)
  const acceptMutation = useAcceptFriendRequest()
  const rejectMutation = useRejectFriendRequest()

  const incoming = requestsQuery.data?.incoming ?? []

  const onAccept = (id: number) => {
    acceptMutation.mutate(id, {
      onSuccess: () => toast.success(t('userFriends.requestAccepted')),
    })
  }

  const onReject = (id: number) => {
    rejectMutation.mutate(id, {
      onSuccess: () => toast.success(t('userFriends.requestRejected')),
    })
  }

  return (
    <div className="flex h-full flex-col">
      <div className="flex items-center gap-2 border-b bg-muted/20 px-3 py-2">
        <Button variant="ghost" size="icon" onClick={onBack} className="size-7 shrink-0">
          <ArrowLeftIcon className="size-4" />
        </Button>
        <span className="text-sm font-semibold">
          {t('chat.friendRequests')} ({incoming.length})
        </span>
      </div>

      <div className="flex-1 space-y-2 overflow-y-auto p-3">
        {incoming.length === 0 ? (
          <p className="p-4 text-center text-xs text-muted-foreground">{t('userFriends.noIncoming')}</p>
        ) : (
          incoming.map((req) => (
            <div key={req.id} className="flex flex-col gap-2 rounded-lg border p-2.5">
              <div className="flex items-center gap-2.5">
                <UserAvatar user={req.user} className="size-8 text-xs" />
                <div className="min-w-0 flex-1">
                  <p className="truncate text-xs font-medium">{req.user.displayName}</p>
                  <p className="text-[0.68rem] text-muted-foreground">@{req.user.username}</p>
                </div>
              </div>
              {req.message && <p className="rounded bg-muted/50 p-1.5 text-xs text-muted-foreground italic">"{req.message}"</p>}
              <div className="flex items-center justify-end gap-1.5 pt-1">
                <Button size="sm" onClick={() => onAccept(req.id)} disabled={acceptMutation.isPending} className="h-7 text-xs">
                  <CheckIcon className="mr-1 size-3" />
                  {t('userFriends.accept')}
                </Button>
                <Button
                  variant="outline"
                  size="sm"
                  onClick={() => onReject(req.id)}
                  disabled={rejectMutation.isPending}
                  className="h-7 text-xs"
                >
                  {t('userFriends.reject')}
                </Button>
              </div>
            </div>
          ))
        )}
      </div>
    </div>
  )
}

export function AddFriendModal({ open, onOpenChange }: { open: boolean; onOpenChange: (open: boolean) => void }) {
  const { t, i18n } = useTranslation()
  const sendMutation = useSendFriendRequest()
  const [username, setUsername] = useState('')
  const [message, setMessage] = useState('')

  const onSubmit = () => {
    const trimmed = username.trim()
    if (!trimmed) return
    sendMutation.mutate(
      { to: trimmed, message: message.trim() },
      {
        onSuccess: () => {
          toast.success(t('chat.requestSent'))
          setUsername('')
          setMessage('')
          onOpenChange(false)
        },
        onError: (error) => toast.error(errorMessage(i18n, error)),
      },
    )
  }

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-xs">
        <DialogHeader>
          <DialogTitle>{t('chat.addFriend')}</DialogTitle>
          <DialogDescription>{t('chat.addFriendPrompt')}</DialogDescription>
        </DialogHeader>
        <div className="space-y-3 py-2">
          <Input value={username} onChange={(e) => setUsername(e.target.value)} placeholder="Username" autoFocus />
          <Input
            value={message}
            onChange={(e) => setMessage(e.target.value)}
            placeholder={t('userFriends.messagePlaceholder')}
            maxLength={200}
          />
        </div>
        <DialogFooter>
          <Button variant="outline" size="sm" onClick={() => onOpenChange(false)}>
            {t('userFriends.cancel')}
          </Button>
          <Button size="sm" onClick={onSubmit} disabled={!username.trim() || sendMutation.isPending}>
            {t('userFriends.addFriend')}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}

export function CreateGroupModal({
  open,
  onOpenChange,
  onCreated,
}: {
  open: boolean
  onOpenChange: (open: boolean) => void
  onCreated: (channelId: string) => void
}) {
  const { t, i18n } = useTranslation()
  const friendsQuery = useUserFriends(open)
  const createMutation = useCreateChatGroup()
  const [name, setName] = useState('')
  const [selectedIds, setSelectedIds] = useState<number[]>([])

  const friends = friendsQuery.data?.friends ?? []

  const toggleMember = (id: number) => {
    setSelectedIds((prev) => (prev.includes(id) ? prev.filter((i) => i !== id) : [...prev, id]))
  }

  const onSubmit = () => {
    const trimmed = name.trim()
    if (!trimmed) return
    if (selectedIds.length === 0) {
      toast.error(t('chat.selectFriendsHint'))
      return
    }

    createMutation.mutate(
      { name: trimmed, memberIds: selectedIds },
      {
        onSuccess: (channel) => {
          toast.success(t('chat.createGroupSuccess'))
          setName('')
          setSelectedIds([])
          onOpenChange(false)
          onCreated(channel.id)
        },
        onError: (error) => toast.error(chatError(i18n, error)),
      },
    )
  }

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-xs">
        <DialogHeader>
          <DialogTitle>{t('chat.createGroup')}</DialogTitle>
          <DialogDescription>{t('chat.selectFriends')}</DialogDescription>
        </DialogHeader>

        <div className="space-y-3 py-2">
          <Input value={name} onChange={(e) => setName(e.target.value)} placeholder={t('chat.groupName')} maxLength={50} autoFocus />

          <div className="max-h-48 space-y-1.5 overflow-y-auto rounded-md border p-2">
            {friends.length === 0 ? (
              <p className="p-2 text-xs text-muted-foreground">{t('chat.noDirectFriends')}</p>
            ) : (
              friends.map((f) => (
                <button
                  key={f.id}
                  type="button"
                  onClick={() => toggleMember(f.id)}
                  className="flex w-full cursor-pointer items-center gap-2.5 rounded-md p-1.5 text-left hover:bg-muted/50"
                >
                  <input
                    type="checkbox"
                    checked={selectedIds.includes(f.id)}
                    onChange={() => {}}
                    className="pointer-events-none size-4 rounded border-gray-300 text-primary focus:ring-primary"
                  />
                  <UserAvatar user={f} className="size-6 text-[0.65rem]" />
                  <span className="truncate text-xs font-medium">{f.displayName}</span>
                </button>
              ))
            )}
          </div>
        </div>

        <DialogFooter>
          <Button variant="outline" size="sm" onClick={() => onOpenChange(false)}>
            {t('userFriends.cancel')}
          </Button>
          <Button size="sm" onClick={onSubmit} disabled={!name.trim() || createMutation.isPending}>
            {t('chat.createGroup')}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
