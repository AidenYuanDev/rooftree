import { cn } from '~/lib/utils'

// Above the footer: the hillside of the painting, falling to the right, with a far ridge behind it.
// Its band below in --ground

export function Hill({ className }: { className?: string }) {
  return (
    <div aria-hidden className={cn('relative h-20 md:h-28', className)}>
      <svg viewBox="0 0 1440 120" preserveAspectRatio="none" className="absolute inset-0 size-full">
        <path d="M0 70C220 52 420 66 640 60S1080 40 1440 58V120H0Z" fill="color-mix(in oklch, var(--ground) 55%, transparent)" />
        <path d="M0 34C260 40 520 58 820 76S1240 100 1440 104V120H0Z" fill="var(--ground)" />
      </svg>
    </div>
  )
}
