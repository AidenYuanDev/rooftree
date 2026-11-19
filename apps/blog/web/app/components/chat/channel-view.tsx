import {
  ArrowLeftIcon,
  BellIcon,
  BellOffIcon,
  EraserIcon,
  InfoIcon,
  MoreHorizontalIcon,
  PinIcon,
  PinOffIcon,
  SearchIcon,
  ShieldIcon,
  Trash2Icon,
  UploadIcon,
  UsersIcon,
} from 'lucide-react'
import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react'
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
import { Dialog, DialogContent, DialogHeader, DialogTitle } from '~/components/ui/dialog'
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from '~/components/ui/dropdown-menu'
import { Skeleton } from '~/components/ui/skeleton'
import { UserAvatar } from '~/components/user-avatar'
import { errorMessage } from '~/i18n'
import { localePath, useLang } from '~/lib/lang'
import {
  useChatGroupMembers,
  useChatMessages,
  useClearChatChannel,
  useDeleteChatMessage,
  useDissolveChatGroup,
  useMarkChatRead,
  useOlderChatMessages,
  useRecallChatMessage,
  useSetChatPref,
  useUserFriends,
} from '~/lib/queries'
import type { ChatChannelItem, ChatMessageItem } from '~/lib/schemas'
import { cn } from '~/lib/utils'
import { useChatStore } from '~/stores/chat'
import { useWatch } from '~/stores/live'
import { channelTitle, chatError, continuesGroup, groupIdOf } from './chat-utils'
import { Composer, type MentionCandidate } from './composer'
import { GroupPanel } from './group-panel'
import { MessageBubble } from './message-bubble'
import { SearchPanel } from './search-panel'

// Pages of 200 going back to a message, at most this many (the channels keep up to 5000)
const MAX_JUMP_PAGES = 30

// One chat: its header (what it is, its settings, a moderator's menu), its messages with older ones
// on demand, and the box to write in. The dock shows it alone (back to the list, members and search
// in place); the chat page has its details and search beside it (onDetails, onSearch)
export function ChannelView({
  channel,
  channels,
  currentUser,
  moderator,
  variant,
  onBack,
  onLeft,
  onDetails,
  onSearch,
  jumpTo,
  onJumped,
}: {
  channel: ChatChannelItem
  // Every chat of mine, for the search's names
  channels: ChatChannelItem[]
  currentUser: { id: number; username: string }
  // May remove messages here: a moderator in the square or a group, never a direct chat
  moderator: boolean
  variant: 'dock' | 'page'
  onBack: () => void
  // Out of a group (left, dissolved)
  onLeft: () => void
  onDetails?: () => void
  onSearch?: () => void
  // A message to go to (a search's, a quote's), and done
  jumpTo?: number | null
  onJumped?: () => void
}) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const messagesQuery = useChatMessages(channel.id)
  const markReadMutation = useMarkChatRead()
  const { older, load: loadOlder } = useOlderChatMessages(channel.id)
  const deleteMessage = useDeleteChatMessage()
  const recallMessage = useRecallChatMessage()
  const clearChannel = useClearChatChannel()
  const dissolveGroup = useDissolveChatGroup()
  const setPref = useSetChatPref()
  const setViewing = useChatStore((state) => state.setViewing)
  const isGroup = channel.type === 'group'
  const groupId = groupIdOf(channel.id)
  const members = useChatGroupMembers(isGroup ? groupId : null)
  const friends = useUserFriends(channel.type === 'square')

  useWatch(`chat:${channel.id}`)
  useEffect(() => {
    setViewing(channel.id)
    return () => setViewing(null)
  }, [channel.id, setViewing])

  const [confirm, setConfirm] = useState<'clear' | 'dissolve' | null>(null)
  const [loadingOlder, setLoadingOlder] = useState(false)
  // The dock's own panels; the page has them beside the chat
  const [panel, setPanel] = useState<'members' | 'search' | null>(null)
  const [replyTo, setReplyTo] = useState<ChatMessageItem | null>(null)
  const [editing, setEditing] = useState<ChatMessageItem | null>(null)
  const [pendingFile, setPendingFile] = useState<File | null>(null)
  const [dragging, setDragging] = useState(false)
  const [highlighted, setHighlighted] = useState<number | null>(null)
  const scrollRef = useRef<HTMLDivElement>(null)
  // The list's height before older messages went on top, to keep the reader where they were
  const heightBeforeOlder = useRef<number | null>(null)
  // Near the bottom: new messages scroll into view
  const atBottom = useRef(true)

  const latest = useMemo(() => messagesQuery.data?.messages ?? [], [messagesQuery.data])
  const messages = useMemo(
    () => (older ? [...older.messages.filter((m) => !latest.some((l) => l.id === m.id)), ...latest] : latest),
    [older, latest],
  )
  const hasMore = older ? older.hasMore : (messagesQuery.data?.hasMore ?? false)

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

  // A new message at the bottom: shown, when the reader was there or sent it
  const newest = latest.at(-1)
  useEffect(() => {
    const list = scrollRef.current
    if (list && (atBottom.current || newest?.senderId === currentUser.id)) list.scrollTop = list.scrollHeight
  }, [newest?.id, newest?.senderId, currentUser.id])

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

  // Going to a message: older pages loaded until it is there, then it is scrolled to and marked a while
  const goTo = async (messageId: number) => {
    let loaded = messages
    let more = hasMore
    for (let page = 0; !loaded.some((m) => m.id === messageId) && more && page < MAX_JUMP_PAGES; ++page) {
      const oldest = loaded.find((m) => m.id > 0)
      if (!oldest) break
      try {
        const merged = await loadOlder(oldest.id, 200)
        loaded = [...merged.messages, ...latest]
        more = merged.hasMore
      } catch (error) {
        toast.error(errorMessage(i18n, error))
        return
      }
    }
    if (!loaded.some((m) => m.id === messageId)) {
      toast.error(t('chat.jumpFailed'))
      return
    }
    setHighlighted(messageId)
  }
  useLayoutEffect(() => {
    if (highlighted === null) return
    document.getElementById(`chat-message-${highlighted}`)?.scrollIntoView({ block: 'center' })
    const timer = setTimeout(() => setHighlighted(null), 2000)
    return () => clearTimeout(timer)
  }, [highlighted])

  // A jump asked for from outside (the page's search, a link), once the messages are there. The latest
  // goTo and onJumped for it, which change every render
  const latestJump = useRef({ goTo, onJumped })
  useEffect(() => {
    latestJump.current = { goTo, onJumped }
  })
  const jumpRequest = messagesQuery.isSuccess ? jumpTo : null
  const jumping = useRef<number | null>(null)
  useEffect(() => {
    if (!jumpRequest || jumping.current === jumpRequest) return
    jumping.current = jumpRequest
    void latestJump.current.goTo(jumpRequest).finally(() => {
      jumping.current = null
      latestJump.current.onJumped?.()
    })
  }, [jumpRequest])

  const onDeleteMessage = (message: ChatMessageItem) =>
    deleteMessage.mutate(message, {
      onSuccess: () => toast.success(t('chat.messageDeleted')),
      onError: (error) => toast.error(errorMessage(i18n, error)),
    })

  const onRecall = (message: ChatMessageItem) =>
    recallMessage.mutate(message.id, {
      onSuccess: () => {
        toast.success(t('chat.messageRecalled'))
        if (replyTo?.id === message.id) setReplyTo(null)
      },
      onError: (error) => toast.error(chatError(i18n, error)),
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
          onLeft()
        },
        onError: (error) => toast.error(errorMessage(i18n, error)),
      })
    setConfirm(null)
  }

  const togglePref = (pref: { pinned?: boolean; muted?: boolean }) =>
    setPref.mutate({ channelId: channel.id, ...pref }, { onError: (error) => toast.error(errorMessage(i18n, error)) })

  // Who "@" may name here: a group's members, the friend, or for the square my friends
  const candidates: MentionCandidate[] = useMemo(() => {
    const list: MentionCandidate[] =
      channel.type === 'group'
        ? (members.data?.members ?? [])
        : channel.type === 'direct'
          ? channel.friend
            ? [channel.friend]
            : []
          : (friends.data?.friends ?? [])
    return list.filter((c) => c.id !== currentUser.id)
  }, [channel, members.data, friends.data, currentUser.id])

  const showSearch = () => (onSearch ? onSearch() : setPanel(panel === 'search' ? null : 'search'))
  const showDetails = () => (onDetails ? onDetails() : setPanel('members'))

  const title = channelTitle(channel, t('chat.squareTitle'))

  return (
    <div
      className="relative flex h-full min-h-0 flex-col"
      onDragOver={(e) => {
        if (!e.dataTransfer.types.includes('Files')) return
        e.preventDefault()
        setDragging(true)
      }}
      onDragLeave={(e) => {
        if (!e.currentTarget.contains(e.relatedTarget as Node | null)) setDragging(false)
      }}
      onDrop={(e) => {
        const file = e.dataTransfer.files[0]
        if (!file) return
        e.preventDefault()
        setDragging(false)
        setEditing(null)
        setPendingFile(file)
      }}
    >
      {/* Header */}
      <div className="flex items-center justify-between gap-2 border-b bg-muted/20 px-3 py-2">
        <div className="flex min-w-0 items-center gap-2">
          <Button
            variant="ghost"
            size="icon"
            onClick={onBack}
            className={cn('size-7 shrink-0', variant === 'page' ? 'md:hidden' : '')}
            aria-label={t('chat.back')}
          >
            <ArrowLeftIcon className="size-4" />
          </Button>
          {channel.type === 'direct' ? (
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
              <span className="truncate text-sm font-semibold">{title}</span>
            </Link>
          ) : (
            <button
              type="button"
              onClick={isGroup ? showDetails : undefined}
              className={cn('flex min-w-0 items-center gap-2', isGroup ? 'hover:opacity-80' : 'cursor-default')}
            >
              <UsersIcon className="size-4 shrink-0 text-primary" />
              <span className="truncate text-sm font-semibold">{title}</span>
              {isGroup && <span className="text-xs text-muted-foreground">({channel.memberCount})</span>}
            </button>
          )}
          {channel.muted && <BellOffIcon className="size-3.5 shrink-0 text-muted-foreground" aria-label={t('chat.mutedLabel')} />}
        </div>

        <div className="flex shrink-0 items-center">
          <Button
            variant="ghost"
            size="icon"
            className="size-7 text-muted-foreground"
            onClick={showSearch}
            title={t('chat.searchMessages')}
            aria-label={t('chat.searchMessages')}
          >
            <SearchIcon className="size-4" />
          </Button>
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button variant="ghost" size="icon" className="size-7 text-muted-foreground" aria-label={t('chat.chatSettings')}>
                <MoreHorizontalIcon className="size-4" />
              </Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent align="end" className="w-40">
              <DropdownMenuItem onSelect={() => togglePref({ pinned: !channel.pinned })}>
                {channel.pinned ? <PinOffIcon className="mr-2 size-4" /> : <PinIcon className="mr-2 size-4" />}
                {channel.pinned ? t('chat.unpin') : t('chat.pin')}
              </DropdownMenuItem>
              <DropdownMenuItem onSelect={() => togglePref({ muted: !channel.muted })}>
                {channel.muted ? <BellIcon className="mr-2 size-4" /> : <BellOffIcon className="mr-2 size-4" />}
                {channel.muted ? t('chat.unmute') : t('chat.mute')}
              </DropdownMenuItem>
              {(isGroup || variant === 'page') && (
                <>
                  <DropdownMenuSeparator />
                  <DropdownMenuItem onSelect={showDetails}>
                    <InfoIcon className="mr-2 size-4" />
                    {t('chat.details')}
                  </DropdownMenuItem>
                </>
              )}
            </DropdownMenuContent>
          </DropdownMenu>
          {moderator && (
            <DropdownMenu>
              <DropdownMenuTrigger asChild>
                <Button variant="ghost" size="icon" className="size-7 text-muted-foreground" aria-label={t('chat.moderate')}>
                  <ShieldIcon className="size-4" />
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

      {/* The dock's members, in a dialog */}
      {isGroup && variant === 'dock' && (
        <Dialog open={panel === 'members'} onOpenChange={(open) => setPanel(open ? 'members' : null)}>
          <DialogContent className="flex h-[min(32rem,80dvh)] max-w-xs flex-col gap-0 p-0">
            <DialogHeader className="sr-only">
              <DialogTitle>{t('chat.details')}</DialogTitle>
            </DialogHeader>
            <GroupPanel channel={channel} currentUserId={currentUser.id} onLeft={onLeft} />
          </DialogContent>
        </Dialog>
      )}

      <div className="relative flex min-h-0 flex-1 flex-col">
        {/* The dock's search, over the messages */}
        {panel === 'search' && (
          <div className="absolute inset-0 z-20 bg-background">
            <SearchPanel
              channel={channel}
              channels={channels}
              onClose={() => setPanel(null)}
              onPick={(message) => {
                setPanel(null)
                if (message.channelId === channel.id) void goTo(message.id)
                else useChatStore.getState().openChannel(message.channelId)
              }}
            />
          </div>
        )}

        {/* Messages */}
        <div
          ref={scrollRef}
          onScroll={(e) => {
            const list = e.currentTarget
            atBottom.current = list.scrollHeight - list.scrollTop - list.clientHeight < 80
          }}
          className="min-h-0 flex-1 space-y-3 overflow-x-hidden overflow-y-auto p-3"
        >
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
            messages.map((m, index) => (
              <MessageBubble
                key={m.id}
                msg={m}
                isMe={m.senderId === currentUser.id}
                myUsername={currentUser.username}
                continued={continuesGroup(messages[index - 1], m)}
                highlighted={highlighted === m.id}
                actions={{
                  onReply: (message) => {
                    setEditing(null)
                    setReplyTo(message)
                  },
                  onEdit: (message) => {
                    setReplyTo(null)
                    setPendingFile(null)
                    setEditing(message)
                  },
                  onRecall,
                  onDelete: moderator ? onDeleteMessage : undefined,
                  onJumpTo: (id) => void goTo(id),
                }}
              />
            ))
          )}
        </div>

        {dragging && (
          <div className="pointer-events-none absolute inset-0 z-30 flex items-center justify-center rounded-md border-2 border-dashed border-primary bg-background/80">
            <span className="flex items-center gap-2 text-sm font-medium text-primary">
              <UploadIcon className="size-5" />
              {t('chat.dropFile')}
            </span>
          </div>
        )}

        <Composer
          key={channel.id}
          channelId={channel.id}
          replyTo={replyTo}
          onCancelReply={() => setReplyTo(null)}
          editing={editing}
          onCancelEdit={() => setEditing(null)}
          pendingFile={pendingFile}
          onPendingFile={setPendingFile}
          candidates={candidates}
          autoFocus={variant === 'page'}
        />
      </div>
    </div>
  )
}
