import {
  ArrowLeftIcon,
  CheckIcon,
  EraserIcon,
  LogOutIcon,
  MessageCircleIcon,
  MoreHorizontalIcon,
  PlusIcon,
  SendIcon,
  SmileIcon,
  Trash2Icon,
  UserCheckIcon,
  UserPlusIcon,
  UsersIcon,
  XIcon,
} from 'lucide-react'
import { useQueryClient } from '@tanstack/react-query'
import { useEffect, useLayoutEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import type { i18n } from 'i18next'
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
import { Card, CardContent, CardHeader, CardTitle } from '~/components/ui/card'
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '~/components/ui/dialog'
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from '~/components/ui/dropdown-menu'
import { Input } from '~/components/ui/input'
import { Skeleton } from '~/components/ui/skeleton'
import { Textarea } from '~/components/ui/textarea'
import { UserAvatar } from '~/components/user-avatar'
import { errorMessage, fieldErrorMessage } from '~/i18n'
import { ApiError } from '~/lib/api'
import { localePath, useLang } from '~/lib/lang'
import {
  useAcceptFriendRequest,
  useChatChannels,
  useChatGroupMembers,
  useChatMessages,
  useClearChatChannel,
  useDeleteChatMessage,
  useDissolveChatGroup,
  useChatUnreadCount,
  useCreateChatGroup,
  useLeaveChatGroup,
  useMarkChatRead,
  useOlderChatMessages,
  useMe,
  useRejectFriendRequest,
  useSendChatMessage,
  useSendFriendRequest,
  useUserFriends,
  useUserFriendRequests,
} from '~/lib/queries'
import { canModerate, type ChatChannelItem, type ChatMessageItem } from '~/lib/schemas'
import { useChatStore } from '~/stores/chat'
import { useWatch } from '~/stores/live'

const QUICK_EMOJIS = ['😊', '👍', '❤️', '🐱', '⭐', '🍵', '🎉', '💡', '👋', '👀']

// A toast's text for a failed chat action: the field's own reason ("too long") rather than "check the form"
function chatError(i18n: i18n, error: unknown) {
  const field = error instanceof ApiError ? Object.values(error.fields)[0] : undefined
  return field ? fieldErrorMessage(i18n, field) : errorMessage(i18n, error)
}

function ChannelList({
  channels,
  onSelect,
  activeId,
}: {
  channels: ChatChannelItem[]
  onSelect: (id: string) => void
  activeId: string | null
}) {
  const { t } = useTranslation()
  const queryClient = useQueryClient()
  useEffect(() => {
    void queryClient.invalidateQueries({ queryKey: ['chat', 'channels'] })
  }, [queryClient])

  const square = channels.find((c) => c.type === 'square')
  const groups = channels.filter((c) => c.type === 'group')
  const directChats = channels.filter((c) => c.type === 'direct')

  return (
    <div className="flex h-full flex-col space-y-3 overflow-y-auto p-2">
      {/* Square */}
      {square && (
        <button
          type="button"
          onClick={() => onSelect(square.id)}
          className={`flex w-full items-center gap-3 rounded-lg border p-3 text-left transition-colors hover:bg-muted/50 ${
            activeId === square.id ? 'border-primary/30 bg-muted/70' : ''
          }`}
        >
          <div className="flex size-10 items-center justify-center rounded-full bg-primary/10 text-primary">
            <UsersIcon className="size-5" />
          </div>
          <div className="min-w-0 flex-1">
            <div className="flex items-center justify-between gap-1">
              <span className="truncate text-sm font-medium">{t('chat.squareTitle')}</span>
              {square.unreadCount > 0 && (
                <span className="py-0.2 rounded-full bg-primary px-1.5 text-[0.65rem] font-bold text-primary-foreground">
                  {square.unreadCount}
                </span>
              )}
            </div>
            <p className="truncate text-xs text-muted-foreground">
              {square.lastMessage ? `${square.lastMessage.senderName}: ${square.lastMessage.content}` : t('chat.noMessagesYet')}
            </p>
          </div>
        </button>
      )}

      {/* Groups */}
      {groups.length > 0 && (
        <div>
          <h4 className="px-2 text-xs font-semibold tracking-wider text-muted-foreground uppercase">{t('chat.groupTitle')}</h4>
          <div className="mt-1 space-y-1">
            {groups.map((grp) => (
              <button
                key={grp.id}
                type="button"
                onClick={() => onSelect(grp.id)}
                className={`flex w-full items-center gap-3 rounded-lg border p-2.5 text-left transition-colors hover:bg-muted/50 ${
                  activeId === grp.id ? 'border-primary/30 bg-muted/70' : ''
                }`}
              >
                <div className="flex size-9 items-center justify-center rounded-full bg-muted text-muted-foreground">
                  <UsersIcon className="size-4" />
                </div>
                <div className="min-w-0 flex-1">
                  <div className="flex items-center justify-between gap-1">
                    <span className="truncate text-sm font-medium">{grp.name}</span>
                    <div className="flex items-center gap-1.5">
                      <span className="text-[0.68rem] text-muted-foreground">({grp.memberCount})</span>
                      {grp.unreadCount > 0 && (
                        <span className="py-0.2 rounded-full bg-primary px-1.5 text-[0.65rem] font-bold text-primary-foreground">
                          {grp.unreadCount}
                        </span>
                      )}
                    </div>
                  </div>
                  <p className="truncate text-xs text-muted-foreground">
                    {grp.lastMessage ? `${grp.lastMessage.senderName}: ${grp.lastMessage.content}` : t('chat.noMessagesYet')}
                  </p>
                </div>
              </button>
            ))}
          </div>
        </div>
      )}

      {/* Direct chats */}
      <div>
        <h4 className="px-2 text-xs font-semibold tracking-wider text-muted-foreground uppercase">{t('chat.directTitle')}</h4>
        {directChats.length === 0 ? (
          <p className="p-3 text-xs text-muted-foreground">{t('chat.noDirectFriends')}</p>
        ) : (
          <div className="mt-1 space-y-1">
            {directChats.map((ch) => (
              <button
                key={ch.id}
                type="button"
                onClick={() => onSelect(ch.id)}
                className={`flex w-full items-center gap-3 rounded-lg border p-2.5 text-left transition-colors hover:bg-muted/50 ${
                  activeId === ch.id ? 'border-primary/30 bg-muted/70' : ''
                }`}
              >
                <div className="relative">
                  <UserAvatar user={ch.friend!} className="size-9 text-xs" />
                  <span
                    className={`absolute -right-0.5 -bottom-0.5 size-2.5 rounded-full ring-2 ring-background ${
                      ch.friend?.online ? 'animate-pulse bg-emerald-500' : 'bg-muted-foreground/50'
                    }`}
                  />
                </div>
                <div className="min-w-0 flex-1">
                  <div className="flex items-center justify-between gap-1">
                    <span className="truncate text-sm font-medium">{ch.friend?.displayName}</span>
                    {ch.unreadCount > 0 && (
                      <span className="py-0.2 rounded-full bg-primary px-1.5 text-[0.65rem] font-bold text-primary-foreground">
                        {ch.unreadCount}
                      </span>
                    )}
                  </div>
                  <p className="truncate text-xs text-muted-foreground">
                    {ch.lastMessage ? ch.lastMessage.content : t('chat.noMessagesYet')}
                  </p>
                </div>
              </button>
            ))}
          </div>
        )}
      </div>
    </div>
  )
}

// onDelete: a moderator's, for a message the server has (not one still being sent)
function MessageBubble({ msg, isMe, onDelete }: { msg: ChatMessageItem; isMe: boolean; onDelete?: () => void }) {
  const { t } = useTranslation()
  return (
    <div className={`group flex items-center gap-2.5 ${isMe ? 'flex-row-reverse' : 'flex-row'}`}>
      {!isMe && (
        <UserAvatar
          user={{
            id: msg.senderId,
            displayName: msg.senderName,
            avatar: msg.senderAvatar,
          }}
          className="size-7 shrink-0 text-[0.65rem]"
        />
      )}
      <div className={`flex max-w-[78%] min-w-0 flex-col gap-1 ${isMe ? 'items-end' : 'items-start'}`}>
        {!isMe && <span className="truncate text-[0.68rem] text-muted-foreground">{msg.senderName}</span>}
        <div
          className={`max-w-full rounded-2xl px-3.5 py-2 text-sm leading-relaxed [overflow-wrap:anywhere] whitespace-pre-wrap select-text ${
            isMe
              ? 'rounded-tr-sm bg-primary text-primary-foreground shadow-sm'
              : 'rounded-tl-sm border bg-card text-card-foreground shadow-xs'
          }`}
        >
          {msg.content}
        </div>
      </div>
      {onDelete && msg.id > 0 && (
        <Button
          variant="ghost"
          size="icon"
          onClick={onDelete}
          title={t('chat.deleteMessage')}
          aria-label={t('chat.deleteMessage')}
          className="size-6 shrink-0 text-muted-foreground opacity-0 group-hover:opacity-100 hover:text-destructive focus-visible:opacity-100"
        >
          <Trash2Icon className="size-3.5" />
        </Button>
      )}
    </div>
  )
}

function GroupMembersModal({
  groupId,
  open,
  onOpenChange,
  currentUserId,
  onLeaveSuccess,
}: {
  groupId: number
  open: boolean
  onOpenChange: (open: boolean) => void
  currentUserId: number
  onLeaveSuccess: () => void
}) {
  const { t, i18n } = useTranslation()
  const membersQuery = useChatGroupMembers(open ? groupId : null)
  const leaveMutation = useLeaveChatGroup()
  const [confirmLeave, setConfirmLeave] = useState(false)

  const members = membersQuery.data?.members ?? []
  const isOwner = members.some((m) => m.id === currentUserId && m.role === 'owner')

  const onLeave = () => {
    leaveMutation.mutate(groupId, {
      onSuccess: () => {
        toast.success(t(isOwner ? 'chat.groupDissolved' : 'chat.leftGroup'))
        setConfirmLeave(false)
        onOpenChange(false)
        onLeaveSuccess()
      },
      onError: (error) => toast.error(errorMessage(i18n, error)),
    })
  }

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-xs">
        <DialogHeader>
          <DialogTitle>{t('chat.groupMembers', { count: members.length })}</DialogTitle>
        </DialogHeader>

        <div className="max-h-60 space-y-2 overflow-y-auto py-2">
          {membersQuery.isPending ? (
            <Skeleton className="h-16 w-full" />
          ) : (
            members.map((m) => (
              <div key={m.id} className="flex items-center justify-between gap-2 rounded-md p-1.5 hover:bg-muted/40">
                <div className="flex min-w-0 items-center gap-2">
                  <div className="relative shrink-0">
                    <UserAvatar user={m} className="size-7 text-[0.65rem]" />
                    <span
                      className={`absolute -right-0.5 -bottom-0.5 size-2 rounded-full ring-1 ring-background ${
                        m.online ? 'animate-pulse bg-emerald-500' : 'bg-muted-foreground/50'
                      }`}
                    />
                  </div>
                  <span className="truncate text-xs font-medium">{m.displayName}</span>
                </div>
                {m.role === 'owner' && (
                  <span className="rounded-full bg-primary/10 px-2 py-0.5 text-[0.65rem] font-medium text-primary">
                    {t('chat.ownerBadge')}
                  </span>
                )}
              </div>
            ))
          )}
        </div>

        <DialogFooter className="flex-col gap-2 sm:flex-row sm:justify-between">
          <Button
            variant="outline"
            size="sm"
            onClick={() => setConfirmLeave(true)}
            className="text-xs text-destructive hover:bg-destructive/10"
          >
            <LogOutIcon className="mr-1.5 size-3.5" />
            {t(isOwner ? 'chat.dissolveGroup' : 'chat.leaveGroup')}
          </Button>
          <Button size="sm" onClick={() => onOpenChange(false)}>
            {t('collab.done')}
          </Button>
        </DialogFooter>

        <AlertDialog open={confirmLeave} onOpenChange={setConfirmLeave}>
          <AlertDialogContent>
            <AlertDialogHeader>
              <AlertDialogTitle>{t(isOwner ? 'chat.dissolveGroup' : 'chat.leaveGroup')}</AlertDialogTitle>
              <AlertDialogDescription>{t(isOwner ? 'chat.dissolveGroupConfirm' : 'chat.leaveGroupConfirm')}</AlertDialogDescription>
            </AlertDialogHeader>
            <AlertDialogFooter>
              <AlertDialogCancel>{t('userFriends.cancel')}</AlertDialogCancel>
              <AlertDialogAction onClick={onLeave} className="text-destructive-foreground bg-destructive hover:bg-destructive/90">
                {t(isOwner ? 'chat.dissolveGroup' : 'chat.leaveGroup')}
              </AlertDialogAction>
            </AlertDialogFooter>
          </AlertDialogContent>
        </AlertDialog>
      </DialogContent>
    </Dialog>
  )
}

function ChannelView({
  channel,
  onBack,
  currentUserId,
  moderator,
}: {
  channel: ChatChannelItem
  onBack: () => void
  currentUserId: number
  // May remove messages here: a moderator in the square or a group, never a direct chat
  moderator: boolean
}) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const messagesQuery = useChatMessages(channel.id)
  const sendMutation = useSendChatMessage()
  const markReadMutation = useMarkChatRead()
  const { older, load: loadOlder } = useOlderChatMessages(channel.id)
  const deleteMessage = useDeleteChatMessage()
  const clearChannel = useClearChatChannel()
  const dissolveGroup = useDissolveChatGroup()
  const [confirm, setConfirm] = useState<'clear' | 'dissolve' | null>(null)

  useWatch(`chat:${channel.id}`)

  const [text, setText] = useState('')
  const [loadingOlder, setLoadingOlder] = useState(false)
  const [membersOpen, setMembersOpen] = useState(false)
  const scrollRef = useRef<HTMLDivElement>(null)
  // The list's height before older messages went on top, to keep the reader where they were
  const heightBeforeOlder = useRef<number | null>(null)

  const latest = messagesQuery.data?.messages ?? []
  const messages = older ? [...older.messages.filter((m) => !latest.some((l) => l.id === m.id)), ...latest] : latest
  const hasMore = older ? older.hasMore : (messagesQuery.data?.hasMore ?? false)
  const isGroup = channel.type === 'group'
  const groupId = isGroup ? Number(channel.id.replace('group:', '')) : 0

  // The newest message the server has (optimistic ones being sent have negative ids). Marked read once
  // per new id: useMutation's object changes every render, so only its stable mutate is a dependency
  const lastMessageId = messages.reduce((max, m) => (m.id > max ? m.id : max), 0)
  const markRead = markReadMutation.mutate
  const markedRef = useRef<{ channelId: string; id: number } | null>(null)
  useEffect(() => {
    if (lastMessageId <= 0) return
    const marked = markedRef.current
    if (marked && marked.channelId === channel.id && marked.id >= lastMessageId) return
    markedRef.current = { channelId: channel.id, id: lastMessageId }
    markRead({ channelId: channel.id, lastReadId: lastMessageId })
  }, [channel.id, lastMessageId, markRead])

  const newestId = latest.at(-1)?.id
  useEffect(() => {
    if (scrollRef.current) scrollRef.current.scrollTop = scrollRef.current.scrollHeight
  }, [newestId])

  useLayoutEffect(() => {
    const list = scrollRef.current
    if (!list || heightBeforeOlder.current === null) return
    list.scrollTop += list.scrollHeight - heightBeforeOlder.current
    heightBeforeOlder.current = null
  }, [older])

  const onLoadOlder = () => {
    const oldest = messages.find((m) => m.id > 0)
    if (!oldest || loadingOlder) return
    setLoadingOlder(true)
    heightBeforeOlder.current = scrollRef.current?.scrollHeight ?? null
    loadOlder(oldest.id)
      .catch((error: unknown) => {
        heightBeforeOlder.current = null
        toast.error(errorMessage(i18n, error))
      })
      .finally(() => setLoadingOlder(false))
  }

  const onDeleteMessage = (message: ChatMessageItem) =>
    deleteMessage.mutate(message, {
      onSuccess: () => toast.success(t('chat.messageDeleted')),
      onError: (error) => toast.error(errorMessage(i18n, error)),
    })

  const onConfirm = () => {
    if (confirm === 'clear')
      clearChannel.mutate(channel.id, {
        onSuccess: ({ removed }) => toast.success(t('chat.channelCleared', { count: removed })),
        onError: (error) => toast.error(errorMessage(i18n, error)),
      })
    else if (confirm === 'dissolve')
      dissolveGroup.mutate(groupId, {
        onSuccess: () => {
          toast.success(t('chat.groupDissolved'))
          onBack()
        },
        onError: (error) => toast.error(errorMessage(i18n, error)),
      })
    setConfirm(null)
  }

  const onSend = () => {
    const trimmed = text.trim()
    if (!trimmed) return
    // The message shows at once (optimistic), so the box empties at once too; a failed one comes back
    setText('')
    sendMutation.mutate(
      { channelId: channel.id, content: trimmed },
      {
        onError: (error) => {
          setText((current) => current || trimmed)
          toast.error(chatError(i18n, error))
        },
      },
    )
  }

  const onKeyDown = (e: React.KeyboardEvent<HTMLTextAreaElement>) => {
    if (e.key === 'Enter' && !e.shiftKey && !e.nativeEvent.isComposing) {
      e.preventDefault()
      onSend()
    }
  }

  const addEmoji = (emoji: string) => {
    setText((prev) => prev + emoji)
  }

  return (
    <div className="flex h-full flex-col">
      {/* Header */}
      <div className="flex items-center justify-between border-b bg-muted/20 px-3 py-2">
        <div className="flex min-w-0 items-center gap-2">
          <Button variant="ghost" size="icon" onClick={onBack} className="size-7 shrink-0">
            <ArrowLeftIcon className="size-4" />
          </Button>
          {channel.type === 'square' ? (
            <div className="flex min-w-0 items-center gap-2">
              <UsersIcon className="size-4 shrink-0 text-primary" />
              <span className="truncate text-sm font-semibold">{t('chat.squareTitle')}</span>
            </div>
          ) : isGroup ? (
            <button type="button" onClick={() => setMembersOpen(true)} className="flex min-w-0 items-center gap-2 hover:opacity-80">
              <UsersIcon className="size-4 shrink-0 text-primary" />
              <span className="truncate text-sm font-semibold">{channel.name}</span>
              <span className="text-xs text-muted-foreground">({channel.memberCount})</span>
            </button>
          ) : (
            <Link
              to={localePath(lang, `/u/${encodeURIComponent(channel.friend?.username ?? '')}`)}
              className="flex min-w-0 items-center gap-2 hover:underline"
            >
              <div className="relative shrink-0">
                <UserAvatar user={channel.friend!} className="size-6 text-[0.65rem]" />
                <span
                  className={`absolute -right-0.5 -bottom-0.5 size-2 rounded-full ring-1 ring-background ${
                    channel.friend?.online ? 'animate-pulse bg-emerald-500' : 'bg-muted-foreground/50'
                  }`}
                />
              </div>
              <span className="truncate text-sm font-semibold">{channel.friend?.displayName}</span>
            </Link>
          )}
        </div>
        {moderator && (
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button variant="ghost" size="icon" className="size-7 shrink-0 text-muted-foreground" aria-label={t('chat.moderate')}>
                <MoreHorizontalIcon className="size-4" />
              </Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent align="end" className="w-40">
              <DropdownMenuItem onSelect={() => setConfirm('clear')}>
                <EraserIcon className="mr-2 size-4" />
                {t('chat.clearChannel')}
              </DropdownMenuItem>
              {isGroup && (
                <DropdownMenuItem onSelect={() => setConfirm('dissolve')} className="text-destructive">
                  <Trash2Icon className="mr-2 size-4" />
                  {t('chat.dissolveGroup')}
                </DropdownMenuItem>
              )}
            </DropdownMenuContent>
          </DropdownMenu>
        )}
      </div>

      <AlertDialog open={confirm !== null} onOpenChange={(open) => !open && setConfirm(null)}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>{t(confirm === 'dissolve' ? 'chat.dissolveGroup' : 'chat.clearChannel')}</AlertDialogTitle>
            <AlertDialogDescription>
              {t(confirm === 'dissolve' ? 'chat.dissolveGroupConfirm' : 'chat.clearChannelConfirm')}
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>{t('userFriends.cancel')}</AlertDialogCancel>
            <AlertDialogAction onClick={onConfirm} className="text-destructive-foreground bg-destructive hover:bg-destructive/90">
              {t(confirm === 'dissolve' ? 'chat.dissolveGroup' : 'chat.clearChannel')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>

      {isGroup && (
        <GroupMembersModal
          groupId={groupId}
          open={membersOpen}
          onOpenChange={setMembersOpen}
          currentUserId={currentUserId}
          onLeaveSuccess={onBack}
        />
      )}

      {/* Messages */}
      <div ref={scrollRef} className="min-h-0 flex-1 space-y-3 overflow-x-hidden overflow-y-auto p-3">
        {hasMore && (
          <div className="text-center">
            <Button variant="ghost" size="sm" onClick={onLoadOlder} disabled={loadingOlder} className="h-7 text-xs text-muted-foreground">
              {t('chat.loadOlder')}
            </Button>
          </div>
        )}

        {messagesQuery.isPending ? (
          <div className="space-y-3 p-2">
            <Skeleton className="h-10 w-2/3" />
            <Skeleton className="ml-auto h-10 w-1/2" />
            <Skeleton className="h-10 w-3/4" />
          </div>
        ) : messages.length === 0 ? (
          <div className="flex h-full items-center justify-center p-6 text-center text-xs text-muted-foreground">
            {t('chat.noMessagesYet')}
          </div>
        ) : (
          messages.map((m) => (
            <MessageBubble
              key={m.id}
              msg={m}
              isMe={m.senderId === currentUserId}
              onDelete={moderator ? () => onDeleteMessage(m) : undefined}
            />
          ))
        )}
      </div>

      {/* Input */}
      <div className="border-t bg-muted/20 p-2.5">
        <div className="flex items-end gap-1.5 rounded-lg border bg-background p-1.5 focus-within:ring-1 focus-within:ring-primary">
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button variant="ghost" size="icon" className="size-7 shrink-0 text-muted-foreground">
                <SmileIcon className="size-4" />
              </Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent side="top" align="start" className="w-auto p-2">
              <div className="grid grid-cols-5 gap-1">
                {QUICK_EMOJIS.map((emoji) => (
                  <button
                    key={emoji}
                    type="button"
                    onClick={() => addEmoji(emoji)}
                    className="flex size-7 cursor-pointer items-center justify-center rounded-md text-base hover:bg-muted"
                  >
                    {emoji}
                  </button>
                ))}
              </div>
            </DropdownMenuContent>
          </DropdownMenu>

          <Textarea
            value={text}
            onChange={(e) => setText(e.target.value)}
            onKeyDown={onKeyDown}
            placeholder={t('chat.placeholder')}
            rows={1}
            maxLength={1000}
            className="max-h-24 min-h-7 resize-none border-0 p-1 text-sm shadow-none focus-visible:ring-0"
          />

          <Button size="icon" onClick={onSend} disabled={!text.trim() || sendMutation.isPending} className="size-7 shrink-0">
            <SendIcon className="size-3.5" />
          </Button>
        </div>
      </div>
    </div>
  )
}

function FriendRequestsView({ onBack }: { onBack: () => void }) {
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

function AddFriendModal({ open, onOpenChange }: { open: boolean; onOpenChange: (open: boolean) => void }) {
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

function CreateGroupModal({
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

export function ChatDock() {
  const { t } = useTranslation()
  const me = useMe()
  const user = me.data?.user

  const { isOpen, activeChannelId, toggleOpen, setOpen, setActiveChannelId } = useChatStore()

  const channelsQuery = useChatChannels(!!user)
  const unreadQuery = useChatUnreadCount(!!user)
  const friendRequestsQuery = useUserFriendRequests(!!user)

  const [viewRequests, setViewRequests] = useState(false)
  const [addFriendOpen, setAddFriendOpen] = useState(false)
  const [createGroupOpen, setCreateGroupOpen] = useState(false)

  const queryClient = useQueryClient()
  useEffect(() => {
    if (isOpen) {
      void queryClient.invalidateQueries({ queryKey: ['chat', 'channels'] })
      void queryClient.invalidateQueries({ queryKey: ['chat', 'unread'] })
    }
  }, [isOpen, queryClient])

  if (!user) return null

  const channels = channelsQuery.data?.channels ?? []
  const totalUnread = unreadQuery.data?.count ?? 0
  const incomingReqsCount = friendRequestsQuery.data?.incoming.length ?? 0
  const activeChannel = channels.find((c) => c.id === activeChannelId)

  return (
    <div className="fixed right-6 bottom-6 z-50 flex flex-col items-end">
      {/* Expanded Dock */}
      {isOpen && (
        <Card className="mb-3 flex h-[520px] w-[360px] max-w-[calc(100vw-2rem)] animate-in flex-col gap-0 overflow-hidden border border-border/80 bg-background/95 py-0 shadow-2xl backdrop-blur-md transition-all duration-200 fade-in slide-in-from-bottom-4">
          {/* Header */}
          <CardHeader className="flex shrink-0 flex-row items-center justify-between border-b bg-muted/40 px-4 py-2.5 [.border-b]:pb-2.5">
            <CardTitle className="text-sm font-semibold">{t('chat.title')}</CardTitle>
            <div className="flex items-center gap-1">
              {!activeChannel && (
                <>
                  {/* Friend requests button */}
                  <Button
                    variant="ghost"
                    size="icon"
                    onClick={() => setViewRequests(!viewRequests)}
                    className="relative size-7 text-muted-foreground hover:text-foreground"
                    title={t('chat.friendRequests')}
                  >
                    <UserCheckIcon className="size-4" />
                    {incomingReqsCount > 0 && (
                      <span className="absolute -top-0.5 -right-0.5 size-2 animate-pulse rounded-full bg-red-500" />
                    )}
                  </Button>

                  {/* Add action menu (+ button) */}
                  <DropdownMenu>
                    <DropdownMenuTrigger asChild>
                      <Button
                        variant="ghost"
                        size="icon"
                        className="size-7 text-muted-foreground hover:text-foreground"
                        title={t('chat.createGroup')}
                      >
                        <PlusIcon className="size-4" />
                      </Button>
                    </DropdownMenuTrigger>
                    <DropdownMenuContent align="end" className="w-36">
                      <DropdownMenuItem onSelect={() => setAddFriendOpen(true)}>
                        <UserPlusIcon className="mr-2 size-4" />
                        {t('chat.addFriend')}
                      </DropdownMenuItem>
                      <DropdownMenuItem onSelect={() => setCreateGroupOpen(true)}>
                        <UsersIcon className="mr-2 size-4" />
                        {t('chat.createGroup')}
                      </DropdownMenuItem>
                    </DropdownMenuContent>
                  </DropdownMenu>
                </>
              )}

              {/* Close / minimize */}
              <Button
                variant="ghost"
                size="icon"
                onClick={() => setOpen(false)}
                className="size-7 text-muted-foreground hover:text-foreground"
              >
                <XIcon className="size-4" />
              </Button>
            </div>
          </CardHeader>

          <CardContent className="flex-1 overflow-hidden p-0">
            {activeChannel ? (
              <ChannelView
                key={activeChannel.id}
                moderator={canModerate(user) && activeChannel.type !== 'direct'}
                channel={activeChannel}
                onBack={() => setActiveChannelId(null)}
                currentUserId={user.id}
              />
            ) : viewRequests ? (
              <FriendRequestsView onBack={() => setViewRequests(false)} />
            ) : (
              <ChannelList
                channels={channels}
                onSelect={(id) => {
                  setActiveChannelId(id)
                  setViewRequests(false)
                }}
                activeId={activeChannelId}
              />
            )}
          </CardContent>
        </Card>
      )}

      {/* Floating Trigger Button */}
      <Button
        onClick={toggleOpen}
        size="icon"
        className="relative size-12 rounded-full bg-primary text-primary-foreground shadow-lg transition-transform hover:scale-105 active:scale-95"
        aria-label={t('chat.title')}
      >
        <MessageCircleIcon className="size-6" />
        {totalUnread > 0 && (
          <span className="absolute -top-1 -right-1 flex h-5 min-w-5 animate-pulse items-center justify-center rounded-full bg-red-500 px-1 text-[0.65rem] font-bold text-white shadow-sm ring-2 ring-background">
            {totalUnread > 99 ? '99+' : totalUnread}
          </span>
        )}
      </Button>

      {/* Modals */}
      <AddFriendModal open={addFriendOpen} onOpenChange={setAddFriendOpen} />
      <CreateGroupModal open={createGroupOpen} onOpenChange={setCreateGroupOpen} onCreated={(channelId) => setActiveChannelId(channelId)} />
    </div>
  )
}
