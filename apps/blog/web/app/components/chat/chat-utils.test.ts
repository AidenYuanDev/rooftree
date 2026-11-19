import { describe, expect, it } from 'vitest'
import type { ChatChannelItem, ChatMessageItem } from '~/lib/schemas'
import {
  continuesGroup,
  filterChannels,
  insertMention,
  mentionAt,
  messagePreview,
  RECALL_WINDOW_MS,
  sortChannels,
  splitMentions,
  withinWindow,
} from './chat-utils'

function channel(id: string, fields: Partial<ChatChannelItem> = {}): ChatChannelItem {
  return {
    id,
    type: id === 'square' ? 'square' : id.startsWith('group:') ? 'group' : 'direct',
    name: id,
    avatar: null,
    friend: null,
    lastMessage: null,
    unreadCount: 0,
    memberCount: 0,
    mentionCount: 0,
    pinned: false,
    muted: false,
    ownerId: 0,
    ...fields,
  }
}

function message(id: number, fields: Partial<ChatMessageItem> = {}): ChatMessageItem {
  return {
    id,
    channelId: 'square',
    senderId: 1,
    senderName: 'Ann',
    senderAvatar: null,
    content: `message ${id}`,
    createdAt: '2026-11-20T10:00:00.000Z',
    replyTo: null,
    file: null,
    editedAt: null,
    recalled: false,
    mentions: [],
    ...fields,
  }
}

describe('splitMentions', () => {
  it('marks @names the way the server reads them', () => {
    expect(splitMentions('hi @Bob_1, and @cat!')).toEqual([
      { text: 'hi ' },
      { text: '@Bob_1', mention: 'bob_1' },
      { text: ', and ' },
      { text: '@cat', mention: 'cat' },
      { text: '!' },
    ])
  })

  it('leaves out mail addresses, short names and a lone @', () => {
    expect(splitMentions('mail me@example.com or @ab or @')).toEqual([{ text: 'mail me@example.com or @ab or @' }])
  })

  it('keeps the text whole without mentions', () => {
    expect(splitMentions('')).toEqual([])
    expect(splitMentions('plain')).toEqual([{ text: 'plain' }])
  })
})

describe('mentionAt and insertMention', () => {
  it('finds the @name being typed before the cursor', () => {
    expect(mentionAt('hello @bo', 9)).toEqual({ start: 6, query: 'bo' })
    expect(mentionAt('@', 1)).toEqual({ start: 0, query: '' })
    expect(mentionAt('me@example', 10)).toBeNull()
    expect(mentionAt('hello @bob there', 16)).toBeNull()
  })

  it('puts the username in with a space after, the cursor behind it', () => {
    expect(insertMention('hi @bo and more', 3, 6, 'bobby')).toEqual({ text: 'hi @bobby  and more', cursor: 10 })
  })
})

describe('sortChannels', () => {
  it('puts pinned chats first, then the latest active, the square before quiet ones', () => {
    const sorted = sortChannels([
      channel('dm:1:2', { lastMessage: message(5) }),
      channel('group:1', { lastMessage: message(9) }),
      channel('dm:1:3'),
      channel('square'),
      channel('group:2', { pinned: true, lastMessage: message(1) }),
    ])
    expect(sorted.map((c) => c.id)).toEqual(['group:2', 'group:1', 'dm:1:2', 'square', 'dm:1:3'])
  })
})

describe('filterChannels', () => {
  it('finds chats by name, friend name and username', () => {
    const channels = [
      channel('square'),
      channel('group:1', { name: 'Book Club' }),
      channel('dm:1:2', { friend: { id: 2, username: 'bobby', displayName: '小张', avatar: null, online: false, since: '' } }),
    ]
    expect(filterChannels(channels, 'book', '小屋广场').map((c) => c.id)).toEqual(['group:1'])
    expect(filterChannels(channels, 'BOBBY', '小屋广场').map((c) => c.id)).toEqual(['dm:1:2'])
    expect(filterChannels(channels, '广场', '小屋广场').map((c) => c.id)).toEqual(['square'])
    expect(filterChannels(channels, '  ', '小屋广场')).toHaveLength(3)
  })
})

describe('messagePreview', () => {
  const labels = { recalled: 'taken back', file: 'File' }

  it('is the text on one line, or the file, or that it was taken back', () => {
    expect(messagePreview(message(1, { content: 'two\n  lines' }), labels)).toBe('two lines')
    expect(
      messagePreview(message(1, { content: '', file: { name: 'a.pdf', mime: 'application/pdf', size: 1, image: false, url: '' } }), labels),
    ).toBe('[File] a.pdf')
    expect(messagePreview({ content: '', recalled: false, hasFile: true }, labels)).toBe('[File]')
    expect(messagePreview(message(1, { recalled: true }), labels)).toBe('taken back')
  })
})

describe('withinWindow', () => {
  const sent = Date.parse('2026-11-20T10:00:00.000Z')

  it('is true for a sent message still within the window', () => {
    expect(withinWindow(message(1), RECALL_WINDOW_MS, sent + RECALL_WINDOW_MS - 1)).toBe(true)
  })

  it('is false once past it, while being sent, or taken back', () => {
    expect(withinWindow(message(1), RECALL_WINDOW_MS, sent + RECALL_WINDOW_MS)).toBe(false)
    expect(withinWindow(message(-1), RECALL_WINDOW_MS, sent)).toBe(false)
    expect(withinWindow(message(1, { recalled: true }), RECALL_WINDOW_MS, sent)).toBe(false)
  })
})

describe('continuesGroup', () => {
  it("joins one sender's messages within five minutes, never a reply", () => {
    const first = message(1)
    expect(continuesGroup(first, message(2, { createdAt: '2026-11-20T10:04:59.000Z' }))).toBe(true)
    expect(continuesGroup(first, message(2, { createdAt: '2026-11-20T10:05:00.000Z' }))).toBe(false)
    expect(continuesGroup(first, message(2, { senderId: 2 }))).toBe(false)
    expect(continuesGroup(undefined, first)).toBe(false)
    expect(
      continuesGroup(
        first,
        message(2, { replyTo: { id: 1, senderId: 1, senderName: 'Ann', content: '', hasFile: false, recalled: false } }),
      ),
    ).toBe(false)
  })
})
