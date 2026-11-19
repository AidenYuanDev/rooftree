import { DownloadIcon, FileIcon, PencilIcon, ReplyIcon, Trash2Icon, Undo2Icon } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { Button } from '~/components/ui/button'
import { UserAvatar } from '~/components/user-avatar'
import { formatBytes } from '~/lib/drive'
import { useLang } from '~/lib/lang'
import type { ChatMessageItem } from '~/lib/schemas'
import { cn } from '~/lib/utils'
import { EDIT_WINDOW_MS, formatMessageTime, messagePreview, RECALL_WINDOW_MS, splitMentions, withinWindow } from './chat-utils'

// The text with its @mentions marked, mine stronger
function MessageText({ text, myUsername, isMe }: { text: string; myUsername: string; isMe: boolean }) {
  return (
    <>
      {splitMentions(text).map((part, index) =>
        part.mention ? (
          <span
            key={index}
            className={cn(
              'font-medium',
              part.mention === myUsername.toLowerCase()
                ? isMe
                  ? 'underline'
                  : 'rounded bg-amber-200/70 px-0.5 text-amber-950 dark:bg-amber-400/30 dark:text-amber-100'
                : isMe
                  ? 'underline decoration-primary-foreground/50'
                  : 'text-primary',
            )}
          >
            {part.text}
          </span>
        ) : (
          <span key={index}>{part.text}</span>
        ),
      )}
    </>
  )
}

export interface MessageActions {
  onReply?: (message: ChatMessageItem) => void
  onEdit?: (message: ChatMessageItem) => void
  onRecall?: (message: ChatMessageItem) => void
  // A moderator's, for a message the server has (not one still being sent)
  onDelete?: (message: ChatMessageItem) => void
  // To the message a reply quotes
  onJumpTo?: (messageId: number) => void
}

// One message: its sender (unless it follows the same sender's), what it quotes, its file, its text,
// when, and what may be done with it on hover. The text alone is the .rounded-2xl bubble
export function MessageBubble({
  msg,
  isMe,
  myUsername,
  continued = false,
  highlighted = false,
  actions = {},
}: {
  msg: ChatMessageItem
  isMe: boolean
  myUsername: string
  // Follows a message of the same sender: no avatar, no name
  continued?: boolean
  // Just jumped to
  highlighted?: boolean
  actions?: MessageActions
}) {
  const { t } = useTranslation()
  const lang = useLang()
  const sent = msg.id > 0

  if (msg.recalled)
    return (
      <div id={`chat-message-${msg.id}`} className="py-0.5 text-center text-xs text-muted-foreground">
        {isMe ? t('chat.recalledByMe') : t('chat.recalledBy', { name: msg.senderName })}
      </div>
    )

  const canEdit = isMe && actions.onEdit && withinWindow(msg, EDIT_WINDOW_MS)
  const canRecall = isMe && actions.onRecall && withinWindow(msg, RECALL_WINDOW_MS)
  const labels = { recalled: t('chat.recalledShort'), file: t('chat.file') }
  const mentionsMe = !isMe && /@/.test(msg.content) && splitMentions(msg.content).some((p) => p.mention === myUsername.toLowerCase())

  return (
    <div
      id={`chat-message-${msg.id}`}
      className={cn(
        'group flex items-start gap-2.5 rounded-lg transition-colors duration-700',
        isMe ? 'flex-row-reverse' : 'flex-row',
        continued ? '-mt-1.5' : '',
        highlighted ? 'bg-amber-200/40 dark:bg-amber-400/15' : '',
      )}
    >
      {!isMe &&
        (continued ? (
          <span className="w-7 shrink-0" />
        ) : (
          <UserAvatar
            user={{ id: msg.senderId, displayName: msg.senderName, avatar: msg.senderAvatar }}
            className="mt-0.5 size-7 shrink-0 text-[0.65rem]"
          />
        ))}
      <div className={cn('flex max-w-[78%] min-w-0 flex-col gap-1', isMe ? 'items-end' : 'items-start')}>
        {!isMe && !continued && <span className="truncate text-[0.68rem] text-muted-foreground">{msg.senderName}</span>}

        {msg.replyTo && (
          <button
            type="button"
            onClick={() => actions.onJumpTo?.(msg.replyTo!.id)}
            className="max-w-full truncate rounded-md border-l-2 border-primary/50 bg-muted/60 px-2 py-1 text-left text-xs text-muted-foreground hover:bg-muted"
          >
            <span className="font-medium text-foreground/80">{msg.replyTo.senderName}</span>
            {': '}
            {messagePreview(msg.replyTo, labels) || t('chat.file')}
          </button>
        )}

        {msg.file &&
          (msg.file.image ? (
            <a href={msg.file.url} target="_blank" rel="noreferrer" className="block max-w-full">
              <img
                src={msg.file.url}
                alt={msg.file.name}
                loading="lazy"
                className={cn('max-h-64 max-w-full rounded-xl border object-contain shadow-xs', sent ? '' : 'opacity-60')}
              />
            </a>
          ) : (
            <a
              href={sent ? `${msg.file.url}?download=1` : undefined}
              download={msg.file.name}
              className={cn(
                'flex max-w-full items-center gap-2.5 rounded-xl border bg-card px-3 py-2 text-card-foreground shadow-xs hover:bg-muted/50',
                sent ? '' : 'pointer-events-none opacity-60',
              )}
            >
              <FileIcon className="size-8 shrink-0 text-primary" />
              <span className="min-w-0">
                <span className="block truncate text-sm font-medium">{msg.file.name}</span>
                <span className="block text-xs text-muted-foreground">{formatBytes(msg.file.size, lang)}</span>
              </span>
              <DownloadIcon className="size-4 shrink-0 text-muted-foreground" aria-label={t('chat.download')} />
            </a>
          ))}

        {msg.content && (
          <div
            className={cn(
              'max-w-full rounded-2xl px-3.5 py-2 text-sm leading-relaxed [overflow-wrap:anywhere] whitespace-pre-wrap select-text',
              isMe
                ? 'rounded-tr-sm bg-primary text-primary-foreground shadow-sm selection:bg-primary-foreground selection:text-primary'
                : 'rounded-tl-sm border bg-card text-card-foreground shadow-xs',
              mentionsMe ? 'ring-2 ring-amber-300/70 dark:ring-amber-400/40' : '',
              sent ? '' : 'opacity-80',
            )}
          >
            <MessageText text={msg.content} myUsername={myUsername} isMe={isMe} />
          </div>
        )}

        <span className="px-1 text-[0.65rem] text-muted-foreground/80">
          {formatMessageTime(msg.createdAt, lang)}
          {msg.editedAt && ` · ${t('chat.edited')}`}
        </span>
      </div>

      {sent && (
        <div className="mt-1 flex shrink-0 items-center opacity-0 transition-opacity group-hover:opacity-100 focus-within:opacity-100">
          {actions.onReply && (
            <Button
              variant="ghost"
              size="icon"
              onClick={() => actions.onReply!(msg)}
              title={t('chat.reply')}
              aria-label={t('chat.reply')}
              className="size-6 text-muted-foreground"
            >
              <ReplyIcon className="size-3.5" />
            </Button>
          )}
          {canEdit && (
            <Button
              variant="ghost"
              size="icon"
              onClick={() => actions.onEdit!(msg)}
              title={t('chat.edit')}
              aria-label={t('chat.edit')}
              className="size-6 text-muted-foreground"
            >
              <PencilIcon className="size-3.5" />
            </Button>
          )}
          {canRecall && (
            <Button
              variant="ghost"
              size="icon"
              onClick={() => actions.onRecall!(msg)}
              title={t('chat.recall')}
              aria-label={t('chat.recall')}
              className="size-6 text-muted-foreground"
            >
              <Undo2Icon className="size-3.5" />
            </Button>
          )}
          {actions.onDelete && (
            <Button
              variant="ghost"
              size="icon"
              onClick={() => actions.onDelete!(msg)}
              title={t('chat.deleteMessage')}
              aria-label={t('chat.deleteMessage')}
              className="size-6 text-muted-foreground hover:text-destructive"
            >
              <Trash2Icon className="size-3.5" />
            </Button>
          )}
        </div>
      )}
    </div>
  )
}
