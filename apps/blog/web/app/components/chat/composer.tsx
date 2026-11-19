import { FileIcon, PaperclipIcon, PencilIcon, ReplyIcon, SendIcon, SmileIcon, XIcon } from 'lucide-react'
import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { toast } from 'sonner'
import { Button } from '~/components/ui/button'
import { DropdownMenu, DropdownMenuContent, DropdownMenuTrigger } from '~/components/ui/dropdown-menu'
import { Textarea } from '~/components/ui/textarea'
import { UserAvatar } from '~/components/user-avatar'
import { formatBytes } from '~/lib/drive'
import { useLang } from '~/lib/lang'
import { useEditChatMessage, useSendChatMessage } from '~/lib/queries'
import type { ChatMessageItem } from '~/lib/schemas'
import { cn } from '~/lib/utils'
import { chatError, insertMention, mentionAt, messagePreview, QUICK_EMOJIS } from './chat-utils'

export interface MentionCandidate {
  id: number
  username: string
  displayName: string
  avatar?: string | null
}

const MAX_LENGTH = 1000

// What is typed into a chat: text, maybe a reply to a message, maybe a file (picked, pasted or dropped
// on the chat: pendingFile), or a change to one's own message (editing). Enter sends, Shift+Enter is a
// new line, Escape lets go of the reply or the edit
export function Composer({
  channelId,
  replyTo,
  onCancelReply,
  editing,
  onCancelEdit,
  pendingFile,
  onPendingFile,
  candidates,
  autoFocus = false,
}: {
  channelId: string
  replyTo: ChatMessageItem | null
  onCancelReply: () => void
  editing: ChatMessageItem | null
  onCancelEdit: () => void
  pendingFile: File | null
  onPendingFile: (file: File | null) => void
  // Who may be @mentioned here
  candidates: MentionCandidate[]
  autoFocus?: boolean
}) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const sendMutation = useSendChatMessage()
  const editMutation = useEditChatMessage()
  const [text, setText] = useState('')
  const textareaRef = useRef<HTMLTextAreaElement>(null)
  const fileInputRef = useRef<HTMLInputElement>(null)
  // The @mention being typed: where it starts, what's typed, which suggestion is chosen
  const [mention, setMention] = useState<{ start: number; query: string; index: number } | null>(null)

  // Editing starts from the message's text, once per message (not again when the cache changes it)
  const editingId = editing?.id ?? null
  const [editedId, setEditedId] = useState<number | null>(null)
  if (editingId !== editedId) {
    setEditedId(editingId)
    if (editing) setText(editing.content)
  }
  useEffect(() => {
    if (editingId !== null) textareaRef.current?.focus()
  }, [editingId])

  useEffect(() => {
    if (replyTo) textareaRef.current?.focus()
  }, [replyTo])

  // A picture to be sent shows small
  const preview = useMemo(() => (pendingFile?.type.startsWith('image/') ? URL.createObjectURL(pendingFile) : null), [pendingFile])
  useEffect(() => () => (preview ? URL.revokeObjectURL(preview) : undefined), [preview])

  const suggestions = mention
    ? candidates
        .filter((c) => c.username.toLowerCase().startsWith(mention.query) || c.displayName.toLowerCase().includes(mention.query))
        .slice(0, 6)
    : []

  const updateMention = (value: string, cursor: number) => {
    const found = mentionAt(value, cursor)
    setMention(found ? { ...found, index: 0 } : null)
  }

  // Where the cursor goes once the text a suggestion changed is in the box: set in the commit itself,
  // before the next key, which would otherwise land at the end (fast typing, an IME)
  const pendingCursor = useRef<number | null>(null)
  useLayoutEffect(() => {
    const textarea = textareaRef.current
    if (pendingCursor.current === null || !textarea) return
    textarea.focus()
    textarea.setSelectionRange(pendingCursor.current, pendingCursor.current)
    pendingCursor.current = null
  }, [text])

  const chooseMention = (candidate: MentionCandidate) => {
    const textarea = textareaRef.current
    if (!mention || !textarea) return
    const result = insertMention(text, mention.start, textarea.selectionStart, candidate.username)
    pendingCursor.current = result.cursor
    setText(result.text)
    setMention(null)
  }

  const onSend = () => {
    const trimmed = text.trim()
    if (editing) {
      if (!trimmed && !editing.file) return
      if (trimmed === editing.content) return onCancelEdit()
      editMutation.mutate(
        { id: editing.id, content: trimmed },
        {
          onSuccess: () => {
            setText('')
            onCancelEdit()
          },
          onError: (error) => toast.error(chatError(i18n, error)),
        },
      )
      return
    }
    if (!trimmed && !pendingFile) return
    // The message shows at once (optimistic), so the box empties at once too; a failed one comes back
    const file = pendingFile
    const reply = replyTo
    setText('')
    onPendingFile(null)
    onCancelReply()
    sendMutation.mutate(
      { channelId, content: trimmed, replyTo: reply, file },
      {
        onError: (error) => {
          setText((current) => current || trimmed)
          if (file) onPendingFile(file)
          toast.error(chatError(i18n, error))
        },
      },
    )
  }

  const onKeyDown = (e: React.KeyboardEvent<HTMLTextAreaElement>) => {
    if (e.nativeEvent.isComposing) return
    if (suggestions.length > 0 && mention) {
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        e.preventDefault()
        const step = e.key === 'ArrowDown' ? 1 : -1
        setMention({ ...mention, index: (mention.index + step + suggestions.length) % suggestions.length })
        return
      }
      if (e.key === 'Enter' || e.key === 'Tab') {
        e.preventDefault()
        const chosen = suggestions[mention.index] ?? suggestions[0]
        if (chosen) chooseMention(chosen)
        return
      }
      if (e.key === 'Escape') {
        e.preventDefault()
        setMention(null)
        return
      }
    }
    if (e.key === 'Escape') {
      if (editing) {
        setText('')
        onCancelEdit()
      } else if (replyTo) onCancelReply()
      return
    }
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault()
      onSend()
    }
  }

  // A picture pasted is sent like one picked
  const onPaste = (e: React.ClipboardEvent<HTMLTextAreaElement>) => {
    const file = Array.from(e.clipboardData.files)[0]
    if (file && !editing) {
      e.preventDefault()
      onPendingFile(file)
    }
  }

  const labels = { recalled: t('chat.recalledShort'), file: t('chat.file') }
  const pending = sendMutation.isPending || editMutation.isPending

  return (
    <div className="relative border-t bg-muted/20 p-2.5">
      {/* What this message answers, or that it changes one */}
      {(replyTo || editing) && (
        <div className="mb-2 flex items-center gap-2 rounded-md border-l-2 border-primary bg-background/80 px-2 py-1 text-xs">
          {editing ? <PencilIcon className="size-3.5 shrink-0 text-primary" /> : <ReplyIcon className="size-3.5 shrink-0 text-primary" />}
          <span className="min-w-0 flex-1 truncate text-muted-foreground">
            <span className="font-medium text-foreground">
              {editing ? t('chat.editing') : t('chat.replyingTo', { name: replyTo!.senderName })}
            </span>
            {!editing && `: ${messagePreview(replyTo!, labels)}`}
          </span>
          <Button
            variant="ghost"
            size="icon"
            className="size-5 shrink-0"
            aria-label={editing ? t('chat.cancelEdit') : t('chat.cancelReply')}
            onClick={() => {
              if (editing) {
                setText('')
                onCancelEdit()
              } else onCancelReply()
            }}
          >
            <XIcon className="size-3" />
          </Button>
        </div>
      )}

      {/* The file to be sent with it */}
      {pendingFile && !editing && (
        <div className="mb-2 flex items-center gap-2.5 rounded-md border bg-background/80 p-1.5">
          {preview ? (
            <img src={preview} alt={pendingFile.name} className="size-12 rounded object-cover" />
          ) : (
            <FileIcon className="size-8 text-primary" />
          )}
          <span className="min-w-0 flex-1">
            <span className="block truncate text-xs font-medium">{pendingFile.name}</span>
            <span className="block text-[0.68rem] text-muted-foreground">{formatBytes(pendingFile.size, lang)}</span>
          </span>
          <Button variant="ghost" size="icon" className="size-6" aria-label={t('chat.removeFile')} onClick={() => onPendingFile(null)}>
            <XIcon className="size-3.5" />
          </Button>
        </div>
      )}

      {/* Who "@na" may be */}
      {suggestions.length > 0 && mention && (
        <div
          role="listbox"
          className="absolute right-2.5 bottom-full left-2.5 z-10 mb-1 overflow-hidden rounded-md border bg-popover p-1 shadow-md"
        >
          {suggestions.map((c, index) => (
            <button
              key={c.id}
              type="button"
              role="option"
              aria-selected={index === mention.index}
              onMouseDown={(e) => {
                e.preventDefault()
                chooseMention(c)
              }}
              className={cn(
                'flex w-full items-center gap-2 rounded px-2 py-1 text-left text-sm',
                index === mention.index ? 'bg-accent text-accent-foreground' : 'hover:bg-muted',
              )}
            >
              <UserAvatar user={c} className="size-5 text-[0.6rem]" />
              <span className="truncate">{c.displayName}</span>
              <span className="truncate text-xs text-muted-foreground">@{c.username}</span>
            </button>
          ))}
        </div>
      )}

      <div className="flex items-end gap-1 rounded-lg border bg-background p-1.5 focus-within:ring-1 focus-within:ring-primary">
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
                  onClick={() => setText((prev) => prev + emoji)}
                  className="flex size-7 cursor-pointer items-center justify-center rounded-md text-base hover:bg-muted"
                >
                  {emoji}
                </button>
              ))}
            </div>
          </DropdownMenuContent>
        </DropdownMenu>

        {!editing && (
          <>
            <Button
              variant="ghost"
              size="icon"
              className="size-7 shrink-0 text-muted-foreground"
              title={t('chat.attach')}
              aria-label={t('chat.attach')}
              onClick={() => fileInputRef.current?.click()}
            >
              <PaperclipIcon className="size-4" />
            </Button>
            <input
              ref={fileInputRef}
              type="file"
              className="hidden"
              data-testid="chat-file-input"
              onChange={(e) => {
                const file = e.target.files?.[0]
                if (file) onPendingFile(file)
                e.target.value = ''
              }}
            />
          </>
        )}

        <Textarea
          ref={textareaRef}
          value={text}
          onChange={(e) => {
            setText(e.target.value)
            updateMention(e.target.value, e.target.selectionStart)
          }}
          onKeyDown={onKeyDown}
          onPaste={onPaste}
          onBlur={() => setMention(null)}
          onClick={(e) => updateMention(text, e.currentTarget.selectionStart)}
          placeholder={t('chat.placeholder')}
          rows={1}
          maxLength={MAX_LENGTH}
          autoFocus={autoFocus}
          className="max-h-32 min-h-7 resize-none border-0 p-1 text-sm shadow-none focus-visible:ring-0"
        />

        <Button
          size="icon"
          onClick={onSend}
          disabled={(!text.trim() && !(pendingFile && !editing)) || pending}
          className="size-7 shrink-0"
          aria-label={t('chat.sendMessage')}
        >
          <SendIcon className="size-3.5" />
        </Button>
      </div>
    </div>
  )
}
