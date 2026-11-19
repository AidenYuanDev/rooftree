import { BellOffIcon, PinIcon, UsersIcon } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { UserAvatar } from '~/components/user-avatar'
import { useLang } from '~/lib/lang'
import type { ChatChannelItem } from '~/lib/schemas'
import { cn } from '~/lib/utils'
import { channelTitle, formatMessageTime, messagePreview, sortChannels } from './chat-utils'

function ChannelIcon({ channel }: { channel: ChatChannelItem }) {
  if (channel.type === 'direct' && channel.friend)
    return (
      <div className="relative shrink-0">
        <UserAvatar user={channel.friend} className="size-9 text-xs" />
        <span
          className={`absolute -right-0.5 -bottom-0.5 size-2.5 rounded-full ring-2 ring-background ${
            channel.friend.online ? 'animate-pulse bg-emerald-500' : 'bg-muted-foreground/50'
          }`}
        />
      </div>
    )
  return (
    <div
      className={cn(
        'flex size-9 shrink-0 items-center justify-center rounded-full',
        channel.type === 'square' ? 'bg-primary/10 text-primary' : 'bg-muted text-muted-foreground',
      )}
    >
      <UsersIcon className="size-4" />
    </div>
  )
}

// Every chat of mine, pinned first then the latest active; each with its unread (grey when muted), an
// "@" when it mentions me, and its last message
export function ChannelList({
  channels,
  activeId,
  onSelect,
  emptyHint = true,
}: {
  channels: ChatChannelItem[]
  activeId: string | null
  onSelect: (id: string) => void
  // The "no friends yet" line when there's no direct chat
  emptyHint?: boolean
}) {
  const { t } = useTranslation()
  const lang = useLang()
  const labels = { recalled: t('chat.recalledShort'), file: t('chat.file') }
  const hasDirect = channels.some((c) => c.type === 'direct')

  return (
    <div className="flex flex-col gap-1 p-2" data-testid="chat-channel-list">
      {sortChannels(channels).map((channel) => {
        const name = channelTitle(channel, t('chat.squareTitle'))
        const last = channel.lastMessage
        const preview = last
          ? `${channel.type === 'direct' ? '' : `${last.senderName}: `}${messagePreview(last, labels)}`
          : t('chat.noMessagesYet')
        return (
          <button
            key={channel.id}
            type="button"
            onClick={() => onSelect(channel.id)}
            aria-current={activeId === channel.id ? 'true' : undefined}
            className={cn(
              'flex w-full items-center gap-3 rounded-lg border p-2.5 text-left transition-colors hover:bg-muted/50',
              activeId === channel.id ? 'border-primary/30 bg-muted/70' : 'border-transparent',
              channel.pinned ? 'bg-muted/30' : '',
            )}
          >
            <ChannelIcon channel={channel} />
            <div className="min-w-0 flex-1">
              <div className="flex items-center justify-between gap-1">
                <span className="truncate text-sm font-medium">{name}</span>
                <span className="flex shrink-0 items-center gap-1">
                  {channel.type === 'group' && <span className="text-[0.68rem] text-muted-foreground">({channel.memberCount})</span>}
                  {channel.pinned && <PinIcon className="size-3 text-muted-foreground" aria-label={t('chat.pinnedLabel')} />}
                  {channel.muted && <BellOffIcon className="size-3 text-muted-foreground" aria-label={t('chat.mutedLabel')} />}
                  {last && <span className="text-[0.65rem] text-muted-foreground">{formatMessageTime(last.createdAt, lang)}</span>}
                </span>
              </div>
              <div className="flex items-center justify-between gap-1">
                <p className="truncate text-xs text-muted-foreground">
                  {channel.mentionCount > 0 && (
                    <span className="font-medium text-amber-600 dark:text-amber-400">[{t('chat.mentionBadge')}] </span>
                  )}
                  {preview}
                </p>
                {channel.unreadCount > 0 && (
                  <span
                    className={cn(
                      'shrink-0 rounded-full px-1.5 text-[0.65rem] font-bold',
                      channel.muted && channel.mentionCount === 0
                        ? 'bg-muted-foreground/30 text-foreground'
                        : 'bg-primary text-primary-foreground',
                    )}
                  >
                    {channel.unreadCount > 99 ? '99+' : channel.unreadCount}
                  </span>
                )}
              </div>
            </div>
          </button>
        )
      })}
      {emptyHint && !hasDirect && <p className="p-3 text-xs text-muted-foreground">{t('chat.noDirectFriends')}</p>}
    </div>
  )
}
