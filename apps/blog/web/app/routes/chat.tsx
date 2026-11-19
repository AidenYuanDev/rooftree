import { MessageCircleIcon, PlusIcon, SearchIcon, UserCheckIcon, UserPlusIcon, UsersIcon, XIcon } from 'lucide-react'
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Link, useSearchParams } from 'react-router'
import type { Route } from './+types/chat'
import { ChannelList } from '~/components/chat/channel-list'
import { ChannelView } from '~/components/chat/channel-view'
import { filterChannels } from '~/components/chat/chat-utils'
import { AddFriendModal, CreateGroupModal, FriendRequestsView } from '~/components/chat/dialogs'
import { GroupPanel } from '~/components/chat/group-panel'
import { SearchPanel } from '~/components/chat/search-panel'
import { RequireUser } from '~/components/require-user'
import { Button } from '~/components/ui/button'
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from '~/components/ui/dropdown-menu'
import { Input } from '~/components/ui/input'
import { Skeleton } from '~/components/ui/skeleton'
import { UserAvatar } from '~/components/user-avatar'
import { formatDate } from '~/i18n'
import { localePath, useLang } from '~/lib/lang'
import { privateMeta } from '~/lib/page'
import { useChatChannels, useUserFriendRequests } from '~/lib/queries'
import { canModerate, type ChatChannelItem, type User } from '~/lib/schemas'
import { cn } from '~/lib/utils'

// The chat page: every chat of mine on the left, one in the middle, its details or a search on the right.
// ?c= is the chat open, ?m= a message to go to in it (a search's, a mention mail's). A prerendered shell;
// the chats are the browser's to load. Phones show one column at a time

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'chat.pageTitle')

export default function ChatPage() {
  const { t } = useTranslation()
  return <RequireUser prompt={t('chat.loginToChat')}>{(user) => <ChatWorkspace user={user} />}</RequireUser>
}

// What a chat is: a group's members and management, the friend, the square
function ChatDetails({ channel, user, onLeft }: { channel: ChatChannelItem; user: User; onLeft: () => void }) {
  const { t } = useTranslation()
  const lang = useLang()
  if (channel.type === 'group') return <GroupPanel channel={channel} currentUserId={user.id} onLeft={onLeft} />
  if (channel.type === 'direct' && channel.friend)
    return (
      <div className="flex flex-col items-center gap-2 p-6 text-center">
        <UserAvatar user={channel.friend} className="size-16 text-xl" />
        <p className="text-base font-semibold">{channel.friend.displayName}</p>
        <p className="text-xs text-muted-foreground">
          @{channel.friend.username}
          {channel.friend.online && <span className="ml-1.5 text-emerald-600 dark:text-emerald-400">· {t('chat.online')}</span>}
        </p>
        <p className="text-xs text-muted-foreground">{t('chat.friendSince', { date: formatDate(lang, channel.friend.since) })}</p>
        <Button variant="outline" size="sm" asChild className="mt-2">
          <Link to={localePath(lang, `/u/${encodeURIComponent(channel.friend.username)}`)}>{t('chat.viewProfile')}</Link>
        </Button>
        <p className="mt-4 text-xs text-muted-foreground">{t('chat.directInfo')}</p>
      </div>
    )
  return (
    <div className="flex flex-col items-center gap-3 p-6 text-center">
      <div className="flex size-16 items-center justify-center rounded-full bg-primary/10 text-primary">
        <UsersIcon className="size-7" />
      </div>
      <p className="text-base font-semibold">{t('chat.squareTitle')}</p>
      <p className="text-xs text-muted-foreground">{t('chat.squareInfo')}</p>
    </div>
  )
}

function ChatWorkspace({ user }: { user: User }) {
  const { t } = useTranslation()
  const [params, setParams] = useSearchParams()
  const channelsQuery = useChatChannels()
  const requestsQuery = useUserFriendRequests(true)
  const [side, setSide] = useState<'details' | 'search' | null>(null)
  const [viewRequests, setViewRequests] = useState(false)
  const [filter, setFilter] = useState('')
  const [addFriendOpen, setAddFriendOpen] = useState(false)
  const [createGroupOpen, setCreateGroupOpen] = useState(false)

  const channels = channelsQuery.data?.channels ?? []
  const activeId = params.get('c')
  const active = channels.find((c) => c.id === activeId) ?? null
  const jumpTo = Number(params.get('m')) || null
  const incoming = requestsQuery.data?.incoming.length ?? 0

  const open = (channelId: string | null, messageId?: number) => {
    const next = new URLSearchParams()
    if (channelId) next.set('c', channelId)
    if (messageId) next.set('m', String(messageId))
    setParams(next)
    setViewRequests(false)
  }
  const toggleSide = (panel: 'details' | 'search') => setSide((current) => (current === panel ? null : panel))

  return (
    <div className="relative flex h-[calc(100dvh-9rem)] min-h-[32rem] overflow-hidden rounded-xl border bg-background/95 shadow-sm backdrop-blur-md">
      {/* The chats */}
      <aside className={cn('w-full shrink-0 flex-col border-r md:flex md:w-72 lg:w-80', active ? 'hidden' : 'flex')}>
        <div className="flex items-center justify-between gap-2 border-b bg-muted/30 px-3 py-2.5">
          <h1 className="flex items-center gap-2 text-base font-semibold">
            <MessageCircleIcon className="size-4 text-primary" />
            {t('chat.pageTitle')}
          </h1>
          <div className="flex items-center gap-0.5">
            <Button
              variant="ghost"
              size="icon"
              className="size-8 text-muted-foreground"
              onClick={() => toggleSide('search')}
              title={t('chat.searchMessages')}
              aria-label={t('chat.searchMessages')}
            >
              <SearchIcon className="size-4" />
            </Button>
            <Button
              variant="ghost"
              size="icon"
              onClick={() => setViewRequests(!viewRequests)}
              className="relative size-8 text-muted-foreground"
              title={t('chat.friendRequests')}
              aria-label={t('chat.friendRequests')}
            >
              <UserCheckIcon className="size-4" />
              {incoming > 0 && <span className="absolute top-1 right-1 size-2 animate-pulse rounded-full bg-red-500" />}
            </Button>
            <DropdownMenu>
              <DropdownMenuTrigger asChild>
                <Button
                  variant="ghost"
                  size="icon"
                  className="size-8 text-muted-foreground"
                  title={t('chat.createGroup')}
                  aria-label={t('chat.createGroup')}
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
          </div>
        </div>

        {viewRequests ? (
          <FriendRequestsView onBack={() => setViewRequests(false)} />
        ) : (
          <>
            <div className="border-b p-2">
              <div className="relative">
                <SearchIcon className="absolute top-1/2 left-2 size-3.5 -translate-y-1/2 text-muted-foreground" />
                <Input
                  value={filter}
                  onChange={(e) => setFilter(e.target.value)}
                  placeholder={t('chat.filterChats')}
                  aria-label={t('chat.filterChats')}
                  className="h-8 pl-7 text-sm"
                />
              </div>
            </div>
            <div className="min-h-0 flex-1 overflow-y-auto">
              {channelsQuery.isPending ? (
                <div className="space-y-2 p-2">
                  <Skeleton className="h-14 w-full" />
                  <Skeleton className="h-14 w-full" />
                  <Skeleton className="h-14 w-full" />
                </div>
              ) : (
                <ChannelList
                  channels={filterChannels(channels, filter, t('chat.squareTitle'))}
                  activeId={activeId}
                  onSelect={(id) => open(id)}
                  emptyHint={!filter}
                />
              )}
            </div>
          </>
        )}
      </aside>

      {/* The chat open */}
      <section className={cn('min-w-0 flex-1 flex-col', active ? 'flex' : 'hidden md:flex')}>
        {active ? (
          <ChannelView
            key={active.id}
            channel={active}
            channels={channels}
            currentUser={user}
            moderator={canModerate(user) && active.type !== 'direct'}
            variant="page"
            onBack={() => open(null)}
            onLeft={() => {
              setSide(null)
              open(null)
            }}
            onDetails={() => toggleSide('details')}
            onSearch={() => toggleSide('search')}
            jumpTo={jumpTo}
            onJumped={() => open(active.id)}
          />
        ) : (
          <div className="flex flex-1 flex-col items-center justify-center gap-3 p-8 text-center text-sm text-muted-foreground">
            <MessageCircleIcon className="size-10 text-muted-foreground/40" />
            {activeId && channelsQuery.isSuccess ? t('errors.group_not_found') : t('chat.selectChat')}
          </div>
        )}
      </section>

      {/* Beside it: what the chat is, or a search */}
      {side && (side === 'search' || active) && (
        <aside className="absolute inset-y-0 right-0 z-20 flex w-[min(20rem,100%)] flex-col border-l bg-background shadow-lg lg:static lg:shadow-none">
          {/* The search has its own close button */}
          {side === 'details' && (
            <div className="flex items-center justify-between border-b px-3 py-2.5">
              <span className="text-sm font-semibold">{t('chat.details')}</span>
              <Button variant="ghost" size="icon" className="size-7" onClick={() => setSide(null)} aria-label={t('chat.closeDetails')}>
                <XIcon className="size-4" />
              </Button>
            </div>
          )}
          <div className="min-h-0 flex-1">
            {side === 'search' ? (
              <SearchPanel
                key={active?.id ?? 'all'}
                channel={active}
                channels={channels}
                onClose={() => setSide(null)}
                onPick={(message) => open(message.channelId, message.id)}
              />
            ) : (
              active && <ChatDetails channel={active} user={user} onLeft={() => open(null)} />
            )}
          </div>
        </aside>
      )}

      <AddFriendModal open={addFriendOpen} onOpenChange={setAddFriendOpen} />
      <CreateGroupModal open={createGroupOpen} onOpenChange={setCreateGroupOpen} onCreated={(channelId) => open(channelId)} />
    </div>
  )
}
