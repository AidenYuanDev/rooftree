import { FileIcon, SearchIcon, XIcon } from 'lucide-react'
import { useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Button } from '~/components/ui/button'
import { Input } from '~/components/ui/input'
import { Skeleton } from '~/components/ui/skeleton'
import { UserAvatar } from '~/components/user-avatar'
import { useLang } from '~/lib/lang'
import { useChatSearch } from '~/lib/queries'
import type { ChatChannelItem, ChatMessageItem } from '~/lib/schemas'
import { cn } from '~/lib/utils'
import { channelTitle, formatMessageTime } from './chat-utils'

// The text with what was searched for marked
function Highlighted({ text, query }: { text: string; query: string }) {
  const needle = query.trim().toLowerCase()
  if (!needle) return <>{text}</>
  const parts: React.ReactNode[] = []
  const lower = text.toLowerCase()
  let at = 0
  for (let found = lower.indexOf(needle); found !== -1; found = lower.indexOf(needle, at)) {
    if (found > at) parts.push(text.slice(at, found))
    parts.push(
      <mark key={found} className="rounded bg-amber-200/80 px-0.5 text-inherit dark:bg-amber-400/30">
        {text.slice(found, found + needle.length)}
      </mark>,
    )
    at = found + needle.length
  }
  parts.push(text.slice(at))
  return <>{parts}</>
}

// Messages found by their text or file name: in one chat (channel), or in every chat of mine. Picking
// one goes to it in its chat
export function SearchPanel({
  channel,
  channels,
  onPick,
  onClose,
}: {
  // The chat being looked at, when there is one: searched alone unless "all chats" is chosen
  channel: ChatChannelItem | null
  channels: ChatChannelItem[]
  onPick: (message: ChatMessageItem) => void
  onClose: () => void
}) {
  const { t } = useTranslation()
  const lang = useLang()
  const [text, setText] = useState('')
  const [query, setQuery] = useState('')
  const [here, setHere] = useState(!!channel)
  // A search once typing paused
  useEffect(() => {
    const timer = setTimeout(() => setQuery(text), 300)
    return () => clearTimeout(timer)
  }, [text])

  const search = useChatSearch(query, here && channel ? channel.id : null)
  const messages = search.data?.pages.flatMap((page) => page.messages) ?? []
  const channelName = (id: string) => {
    const found = channels.find((c) => c.id === id)
    return found ? channelTitle(found, t('chat.squareTitle')) : ''
  }

  return (
    <div className="flex h-full flex-col">
      <div className="space-y-2 border-b p-2.5">
        <div className="flex items-center gap-1.5">
          <div className="relative flex-1">
            <SearchIcon className="absolute top-1/2 left-2 size-3.5 -translate-y-1/2 text-muted-foreground" />
            <Input
              value={text}
              onChange={(e) => setText(e.target.value)}
              placeholder={t('chat.searchPlaceholder')}
              aria-label={t('chat.searchMessages')}
              maxLength={100}
              autoFocus
              className="h-8 pl-7 text-sm"
            />
          </div>
          <Button variant="ghost" size="icon" className="size-8 shrink-0" onClick={onClose} aria-label={t('chat.closeSearch')}>
            <XIcon className="size-4" />
          </Button>
        </div>
        {channel && (
          <div className="flex gap-1 text-xs">
            {[true, false].map((value) => (
              <button
                key={String(value)}
                type="button"
                onClick={() => setHere(value)}
                className={cn(
                  'rounded-full px-2.5 py-0.5',
                  here === value ? 'bg-primary text-primary-foreground' : 'bg-muted text-muted-foreground hover:text-foreground',
                )}
              >
                {value ? t('chat.searchHere') : t('chat.searchAll')}
              </button>
            ))}
          </div>
        )}
      </div>

      <div className="min-h-0 flex-1 overflow-y-auto p-2">
        {!query.trim() ? null : search.isPending ? (
          <div className="space-y-2">
            <Skeleton className="h-12 w-full" />
            <Skeleton className="h-12 w-full" />
          </div>
        ) : messages.length === 0 ? (
          <p className="p-4 text-center text-xs text-muted-foreground">{t('chat.noResults')}</p>
        ) : (
          <div className="space-y-1">
            {messages.map((m) => (
              <button
                key={m.id}
                type="button"
                onClick={() => onPick(m)}
                className="flex w-full items-start gap-2 rounded-md p-2 text-left hover:bg-muted/60"
              >
                <UserAvatar
                  user={{ id: m.senderId, displayName: m.senderName, avatar: m.senderAvatar }}
                  className="mt-0.5 size-6 shrink-0 text-[0.6rem]"
                />
                <span className="min-w-0 flex-1">
                  <span className="flex items-center justify-between gap-2 text-[0.68rem] text-muted-foreground">
                    <span className="truncate">
                      {m.senderName}
                      {!(here && channel) && ` · ${channelName(m.channelId)}`}
                    </span>
                    <span className="shrink-0">{formatMessageTime(m.createdAt, lang)}</span>
                  </span>
                  {m.content && (
                    <span className="line-clamp-2 text-sm [overflow-wrap:anywhere]">
                      <Highlighted text={m.content} query={query} />
                    </span>
                  )}
                  {m.file && (
                    <span className="flex items-center gap-1 text-xs text-muted-foreground">
                      <FileIcon className="size-3" />
                      <Highlighted text={m.file.name} query={query} />
                    </span>
                  )}
                </span>
              </button>
            ))}
            {search.hasNextPage && (
              <Button
                variant="ghost"
                size="sm"
                className="w-full text-xs"
                onClick={() => void search.fetchNextPage()}
                disabled={search.isFetchingNextPage}
              >
                {t('chat.loadMore')}
              </Button>
            )}
          </div>
        )}
      </div>
    </div>
  )
}
