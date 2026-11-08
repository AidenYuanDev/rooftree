import { createContext } from 'react'
import type { ReactionKind, ReactionState } from './schemas'

export const reactionEmoji: Record<ReactionKind, string> = {
  like: '👍',
  heart: '❤️',
  laugh: '😂',
  party: '🎉',
  cat: '🐱',
  star: '⭐',
  eyes: '👀',
  thinking: '🤔',
  thanks: '🙏',
}

// States loaded for a whole list (comments, thinking), by target; a Reactions inside one doesn't ask itself
export const ReactionsContext = createContext<Record<string, ReactionState> | null>(null)
