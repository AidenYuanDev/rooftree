import type { i18n } from 'i18next'
import { errorMessage, fieldErrorMessage } from '~/i18n'
import { ApiError } from '~/lib/api'
import type { ChatChannelItem, ChatMessageItem } from '~/lib/schemas'

export const QUICK_EMOJIS = ['😊', '👍', '❤️', '🐱', '⭐', '🍵', '🎉', '💡', '👋', '👀', '😂', '🙏', '🔥', '🤔', '👌']

// How long after sending its sender may still edit or take back a message (Blog.Chat.EditWindow and
// RecallWindow's defaults): only whether the buttons show, the server decides
export const EDIT_WINDOW_MS = 15 * 60_000
export const RECALL_WINDOW_MS = 2 * 60_000

// A toast's text for a failed chat action: the field's own reason ("too long") rather than "check the form"
export function chatError(i18n: i18n, error: unknown) {
  const field = error instanceof ApiError ? Object.values(error.fields)[0] : undefined
  return field ? fieldErrorMessage(i18n, field) : errorMessage(i18n, error)
}

// What a chat is called: the square's own name, the friend's, the group's
export function channelTitle(channel: ChatChannelItem, squareTitle: string) {
  return channel.type === 'square' ? squareTitle : channel.type === 'direct' ? (channel.friend?.displayName ?? channel.name) : channel.name
}

export function groupIdOf(channelId: string): number {
  return channelId.startsWith('group:') ? Number(channelId.slice(6)) : 0
}

// The newest activity of a chat, for ordering: its last message's id, 0 without one
function activity(channel: ChatChannelItem) {
  return channel.lastMessage?.id ?? 0
}

// Pinned first, then by their newest message; the square leads the ones without any
export function sortChannels(channels: ChatChannelItem[]): ChatChannelItem[] {
  return [...channels].sort((a, b) => {
    if (a.pinned !== b.pinned) return a.pinned ? -1 : 1
    const byActivity = activity(b) - activity(a)
    if (byActivity !== 0) return byActivity
    if (a.type === 'square' || b.type === 'square') return a.type === 'square' ? -1 : 1
    return a.name.localeCompare(b.name)
  })
}

// Chats whose name, friend or last message has the text
export function filterChannels(channels: ChatChannelItem[], text: string, squareTitle: string): ChatChannelItem[] {
  const needle = text.trim().toLowerCase()
  if (!needle) return channels
  return channels.filter((channel) =>
    [channel.type === 'square' ? squareTitle : channel.name, channel.friend?.username ?? '', channel.friend?.displayName ?? '']
      .join('\n')
      .toLowerCase()
      .includes(needle),
  )
}

export type TextPart = { text: string; mention?: string }

// A message's text in pieces, "@name" ones marked, as the server reads them (not inside a word)
export function splitMentions(text: string): TextPart[] {
  const parts: TextPart[] = []
  const pattern = /(^|[^A-Za-z0-9_])@([A-Za-z0-9_]{3,20})(?![A-Za-z0-9_])/g
  let last = 0
  for (const match of text.matchAll(pattern)) {
    const [, before = '', name = ''] = match
    const start = match.index + before.length
    if (start > last) parts.push({ text: text.slice(last, start) })
    parts.push({ text: `@${name}`, mention: name.toLowerCase() })
    last = start + name.length + 1
  }
  if (last < text.length) parts.push({ text: text.slice(last) })
  return parts
}

// The "@na" being typed just before the cursor, for suggestions: its start and what's typed so far
export function mentionAt(text: string, cursor: number): { start: number; query: string } | null {
  const before = text.slice(0, cursor)
  const match = /(^|[^A-Za-z0-9_])@([A-Za-z0-9_]{0,20})$/.exec(before)
  if (!match) return null
  const typed = match[2] ?? ''
  return { start: cursor - typed.length - 1, query: typed.toLowerCase() }
}

// The text with the mention being typed replaced by "@username ", and where the cursor goes
export function insertMention(text: string, start: number, cursor: number, username: string) {
  const inserted = `@${username} `
  return { text: text.slice(0, start) + inserted + text.slice(cursor), cursor: start + inserted.length }
}

// A one-line summary of a message, for lists and quotes
export function messagePreview(
  message: Pick<ChatMessageItem, 'content' | 'recalled'> & { file?: { name: string } | null; hasFile?: boolean },
  labels: { recalled: string; file: string },
): string {
  if (message.recalled) return labels.recalled
  const text = message.content.replace(/\s+/g, ' ').trim()
  if (text) return text
  if (message.file) return `[${labels.file}] ${message.file.name}`
  if (message.hasFile) return `[${labels.file}]`
  return ''
}

export function withinWindow(message: ChatMessageItem, windowMs: number, now = Date.now()) {
  return message.id > 0 && !message.recalled && now - new Date(message.createdAt).getTime() < windowMs
}

// Two messages in a row from one sender within five minutes: the second without avatar and name
export function continuesGroup(previous: ChatMessageItem | undefined, message: ChatMessageItem) {
  if (!previous || previous.senderId !== message.senderId || message.replyTo) return false
  return new Date(message.createdAt).getTime() - new Date(previous.createdAt).getTime() < 5 * 60_000
}

// "14:05" today, "11月18日 14:05" before, with the year when not this one
export function formatMessageTime(iso: string, lang: string, now = new Date()) {
  const date = new Date(iso)
  const locale = lang === 'zh' ? 'zh-CN' : 'en-US'
  const time = date.toLocaleTimeString(locale, { hour: '2-digit', minute: '2-digit' })
  if (date.toDateString() === now.toDateString()) return time
  const day = date.toLocaleDateString(locale, {
    month: 'short',
    day: 'numeric',
    ...(date.getFullYear() !== now.getFullYear() ? { year: 'numeric' } : {}),
  })
  return `${day} ${time}`
}
