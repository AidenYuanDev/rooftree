import { useState } from 'react'
import { avatarHue, initials } from '~/lib/avatar'
import { cn } from '~/lib/utils'

// An account's picture, or the first character of its name on a color of its own while it has none
// (or the picture doesn't load)
export function UserAvatar({
  user,
  className = 'size-8 text-sm',
}: {
  user: { id: number; displayName: string; avatar?: string | null }
  className?: string
}) {
  const [failed, setFailed] = useState<string | null>(null)
  const shared = cn('inline-flex shrink-0 items-center justify-center overflow-hidden rounded-full select-none', className)

  if (user.avatar && failed !== user.avatar) {
    return (
      <img
        src={user.avatar}
        alt=""
        loading="lazy"
        decoding="async"
        className={cn(shared, 'bg-muted object-cover')}
        onError={() => setFailed(user.avatar ?? null)}
      />
    )
  }

  const hue = avatarHue(user.id)
  return (
    <span aria-hidden className={cn(shared, 'font-medium text-white')} style={{ backgroundColor: `oklch(0.62 0.12 ${hue})` }}>
      {initials(user.displayName)}
    </span>
  )
}
