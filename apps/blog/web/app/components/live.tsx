import { useQueryClient } from '@tanstack/react-query'
import { useEffect, useRef } from 'react'
import { useTranslation } from 'react-i18next'
import { Link, useLocation } from 'react-router'
import { toast } from 'sonner'
import { z } from 'zod'
import { localePath, useLang } from '~/lib/lang'
import { queryKeys, removeChatMessages, sortChatMessages, useMe } from '~/lib/queries'
import { type ChatChannelsList, ChatMessageItem, type ChatMessagesList } from '~/lib/schemas'
import { useLiveStore } from '~/stores/live'

// The page's WebSocket (/ws, BlogWebSocket.h on the server): tells it which page is open, on every
// navigation and every 25 seconds, and hears back how many read this page and the site, and what
// was just published. Reconnects after a while when the connection drops; the pages work without it.
// The server knows the reader by the cookie of the connection's handshake (for the admin's list of who
// is online), so a login or logout connects again.
// With the page goes what it watches (stores/live.ts): when one of those changes, its comments or
// reactions are asked again, or its text's version goes up for its page to load it again

const heartbeat = 25_000

// A moderator removed one message of a channel (id), or all of them
const ChatRemovedPush = z.object({ channelId: z.string(), id: z.number().optional() })

export function Live() {
  const { t } = useTranslation()
  const lang = useLang()
  const { pathname } = useLocation()
  const queryClient = useQueryClient()
  const socket = useRef<WebSocket | null>(null)
  const path = useRef(pathname)
  const setCounts = useLiveStore((state) => state.set)
  const contentChanged = useLiveStore((state) => state.contentChanged)
  const watching = useLiveStore((state) => state.watching)
  const watch = useRef<string[]>([])
  // Who reads: undefined until known, so a logged in reader doesn't connect twice
  const me = useMe()
  const userId = me.isPending ? undefined : (me.data?.user?.id ?? null)

  // Watch caller's own user events (e.g. friend requests, friendship changes)
  const watchUser = userId ? `user:${userId}` : null
  const watchStore = useLiveStore((state) => state.watch)
  useEffect(() => (watchUser ? watchStore(watchUser) : undefined), [watchStore, watchUser])
  // The latest ones for the socket's handlers, which outlive renders
  const latest = useRef({ t, lang })
  useEffect(() => {
    latest.current = { t, lang }
  })

  useEffect(() => {
    if (userId === undefined) return
    let closed = false
    let reconnected = false
    let retry = 1000
    let reconnect: ReturnType<typeof setTimeout>
    let beat: ReturnType<typeof setInterval>

    const connect = () => {
      const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`)
      socket.current = ws
      const sendPage = () => ws.send(JSON.stringify({ type: 'page', path: path.current, watch: watch.current }))
      ws.onopen = () => {
        retry = 1000
        sendPage()
        beat = setInterval(sendPage, heartbeat)
        if (reconnected && watch.current.length > 0) {
          void queryClient.invalidateQueries({ queryKey: ['comments'] })
          void queryClient.invalidateQueries({ queryKey: ['reactions'] })
          for (const target of watch.current) contentChanged(target)
        }
        reconnected = true
      }
      ws.onmessage = (event: MessageEvent<string>) => {
        const message = JSON.parse(event.data) as {
          type: string
          page?: number
          site?: number
          path?: string
          title?: string
          kind?: string
          in?: number | null
          target?: string
          what?: string
        }
        if (message.type === 'presence') setCounts({ page: message.page ?? null, site: message.site ?? null })
        else if (message.type === 'changed' && message.target) {
          // "*": everything watched
          const { target, what } = message
          if (what === 'comments')
            void queryClient.invalidateQueries({ queryKey: target === '*' ? ['comments'] : queryKeys.comments(target) })
          else if (what === 'reactions') void queryClient.invalidateQueries({ queryKey: ['reactions'] })
          else if (what === 'content') contentChanged(target)
          else if (what === 'friends') void queryClient.invalidateQueries({ queryKey: ['user', 'friends'] })
          else if (what === 'chat_unread' || what === 'message' || what === 'chat_channels') {
            void queryClient.invalidateQueries({ queryKey: ['chat', 'channels'] })
            void queryClient.invalidateQueries({ queryKey: ['chat', 'unread'] })
            if (target && target.startsWith('chat:')) {
              const chId = target.slice(5)
              void queryClient.invalidateQueries({ queryKey: ['chat', 'messages', chId] })
            }
          }
        } else if (message.type === 'chat_message') {
          // Checked like an answer of the API: a message that isn't one is left out
          const parsed = ChatMessageItem.safeParse(message)
          if (parsed.success) {
            const msg = parsed.data
            // A channel not loaded yet is read whole when opened, not started from this one message
            queryClient.setQueryData<ChatMessagesList>(['chat', 'messages', msg.channelId, undefined], (old) => {
              if (!old || old.messages.some((m) => m.id === msg.id)) return old
              // Our own message coming back before the answer to sending it replaces its optimistic copy
              const pending = old.messages.find((m) => m.id < 0 && m.senderId === msg.senderId && m.content === msg.content)
              const others = old.messages.filter((m) => m !== pending)
              return { ...old, messages: [...others, msg].sort(sortChatMessages) }
            })
            queryClient.setQueryData<ChatChannelsList>(['chat', 'channels'], (old) => {
              if (!old) return old
              return {
                ...old,
                channels: old.channels.map((ch) =>
                  ch.id === msg.channelId && (!ch.lastMessage || ch.lastMessage.id < msg.id) ? { ...ch, lastMessage: msg } : ch,
                ),
              }
            })
          }
          void queryClient.invalidateQueries({ queryKey: ['chat', 'unread'] })
        } else if (message.type === 'chat_removed') {
          const removed = ChatRemovedPush.safeParse(message)
          if (removed.success) removeChatMessages(queryClient, removed.data.channelId, removed.data.id)
        } else if (message.type === 'chat_channels_changed') {
          void queryClient.invalidateQueries({ queryKey: ['chat', 'channels'] })
        } else if (message.type === 'published' && message.path) {
          const { t: translate, lang: current } = latest.current
          toast(translate('live.published'), {
            description: message.title,
            action: <Link to={localePath(current, message.path)}>{translate('live.read')}</Link>,
            duration: 10_000,
          })
        } else if (message.type === 'restart') {
          // The server stops in that many seconds (a new version going live), or doesn't any more
          if (typeof message.in !== 'number') toast.dismiss('restart')
          else
            toast(latest.current.t('live.restart', { count: message.in }), {
              id: 'restart',
              description: latest.current.t('live.restartHint'),
              duration: (message.in + 30) * 1000,
            })
        }
      }
      ws.onclose = () => {
        clearInterval(beat)
        setCounts({ page: null, site: null })
        if (closed) return
        reconnect = setTimeout(connect, retry)
        retry = Math.min(retry * 2, 60_000)
      }
    }

    connect()
    return () => {
      closed = true
      clearTimeout(reconnect)
      clearInterval(beat)
      socket.current?.close()
    }
  }, [setCounts, contentChanged, queryClient, userId])

  // A new page, or what it watches changed: tell the server right away (after this render's useWatch
  // effects have settled, so navigating sends once instead of twice)
  const watchKey = Object.keys(watching).sort().join(' ')
  useEffect(() => {
    path.current = pathname
    watch.current = watchKey ? watchKey.split(' ') : []
    const timer = setTimeout(() => {
      if (socket.current?.readyState === WebSocket.OPEN)
        socket.current.send(JSON.stringify({ type: 'page', path: path.current, watch: watch.current }))
    }, 0)
    return () => clearTimeout(timer)
  }, [pathname, watchKey])

  return null
}

// "2 reading this page"; nothing until known, or when it's only the reader
export function ReadingNow({ className }: { className?: string }) {
  const { t } = useTranslation()
  const page = useLiveStore((state) => state.page)
  if (!page || page < 2) return null
  return (
    <span className={className}>
      <span className="mr-1.5 inline-block size-1.5 animate-pulse rounded-full bg-emerald-400 align-middle" />
      {t('live.page', { count: page })}
    </span>
  )
}

export function OnlineNow() {
  const { t } = useTranslation()
  const site = useLiveStore((state) => state.site)
  if (!site) return null
  return (
    <span>
      <span className="mr-1.5 inline-block size-1.5 animate-pulse rounded-full bg-emerald-400 align-middle" />
      {t('live.site', { count: site })}
    </span>
  )
}
