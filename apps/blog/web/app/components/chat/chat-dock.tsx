import { Maximize2Icon, MessageCircleIcon, PlusIcon, UserCheckIcon, UserPlusIcon, UsersIcon, XIcon } from 'lucide-react'
import { useQueryClient } from '@tanstack/react-query'
import { useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Link, useLocation } from 'react-router'
import { Button } from '~/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '~/components/ui/card'
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from '~/components/ui/dropdown-menu'
import { localePath, useLang } from '~/lib/lang'
import { useChatChannels, useChatUnreadCount, useMe, useUserFriendRequests } from '~/lib/queries'
import { canModerate } from '~/lib/schemas'
import { useChatStore } from '~/stores/chat'
import { ChannelList } from './channel-list'
import { ChannelView } from './channel-view'
import { AddFriendModal, CreateGroupModal, FriendRequestsView } from './dialogs'

// The floating chat in the corner of every page: the chats, one of them, the friend requests. The chat
// page (routes/chat.tsx) is the same chats with room; the dock isn't shown there
export function ChatDock() {
  const { t } = useTranslation()
  const lang = useLang()
  const { pathname } = useLocation()
  const me = useMe()
  const user = me.data?.user
  const onChatPage = /^\/(zh|en)\/chat\/?$/.test(pathname)

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

  if (!user || onChatPage) return null

  const channels = channelsQuery.data?.channels ?? []
  const totalUnread = unreadQuery.data?.count ?? 0
  const incomingReqsCount = friendRequestsQuery.data?.incoming.length ?? 0
  const activeChannel = channels.find((c) => c.id === activeChannelId)
  const pagePath = localePath(lang, activeChannel ? `/chat?c=${encodeURIComponent(activeChannel.id)}` : '/chat')

  return (
    <div className="fixed right-6 bottom-6 z-50 flex flex-col items-end">
      {isOpen && (
        <Card className="mb-3 flex h-[520px] max-h-[calc(100dvh-6rem)] w-[360px] max-w-[calc(100vw-2rem)] animate-in flex-col gap-0 overflow-hidden border border-border/80 bg-background/95 py-0 shadow-2xl backdrop-blur-md transition-all duration-200 fade-in slide-in-from-bottom-4">
          <CardHeader className="flex shrink-0 flex-row items-center justify-between border-b bg-muted/40 px-4 py-2.5 [.border-b]:pb-2.5">
            <CardTitle className="text-sm font-semibold">{t('chat.title')}</CardTitle>
            <div className="flex items-center gap-1">
              {!activeChannel && (
                <>
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

              <Button
                variant="ghost"
                size="icon"
                asChild
                className="size-7 text-muted-foreground hover:text-foreground"
                title={t('chat.openPage')}
              >
                <Link to={pagePath} onClick={() => setOpen(false)} aria-label={t('chat.openPage')}>
                  <Maximize2Icon className="size-3.5" />
                </Link>
              </Button>

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
                channel={activeChannel}
                channels={channels}
                currentUser={user}
                moderator={canModerate(user) && activeChannel.type !== 'direct'}
                variant="dock"
                onBack={() => setActiveChannelId(null)}
                onLeft={() => setActiveChannelId(null)}
              />
            ) : viewRequests ? (
              <FriendRequestsView onBack={() => setViewRequests(false)} />
            ) : (
              <div className="h-full overflow-y-auto">
                <ChannelList
                  channels={channels}
                  activeId={activeChannelId}
                  onSelect={(id) => {
                    setActiveChannelId(id)
                    setViewRequests(false)
                  }}
                />
              </div>
            )}
          </CardContent>
        </Card>
      )}

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

      <AddFriendModal open={addFriendOpen} onOpenChange={setAddFriendOpen} />
      <CreateGroupModal open={createGroupOpen} onOpenChange={setCreateGroupOpen} onCreated={(channelId) => setActiveChannelId(channelId)} />
    </div>
  )
}
