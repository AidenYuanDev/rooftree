import { useTranslation } from 'react-i18next'
import { cn } from '~/lib/utils'

// The site's mark: the painting's cottage under its tree on the hill, the window lit (it flickers at
// night, scenery.css). Colors from --mark and --lamp. public/favicon.svg is the same drawing

// The crown of the tree: overlapping puffs
const crown = [
  { cx: 22, cy: 9, r: 6.5 },
  { cx: 16.5, cy: 9.5, r: 4 },
  { cx: 27, cy: 11.5, r: 4.5 },
  { cx: 19.5, cy: 4.5, r: 3.5 },
  { cx: 25, cy: 4.5, r: 3.5 },
  { cx: 29, cy: 7.5, r: 2.5 },
]

export function CottageMark({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 32 32" className={cn('overflow-visible', className)} aria-hidden>
      <g fill="var(--mark)">
        {crown.map((puff) => (
          <circle key={`${puff.cx},${puff.cy}`} {...puff} />
        ))}
        {/* The trunk, wider at the foot, a branch into the crown */}
        <path d="M20 26.5 21 14l-3.5-2.5 4.2 1.4L23 10l.2 4.5L23.5 26.5Z" />
        {/* The cottage: roof, walls, chimney */}
        <path d="M2 21.5 6.5 16h10l2.5 5.5Z" />
        <path d="M3.5 21h14v7.5h-14Z" />
        <path d="M7 13.5h1.8v3H7Z" />
        {/* The hill, falling to the right */}
        <path d="M0 27.5Q12 25.5 32 28.5V32H0Z" />
      </g>
      <path className="scenery-mark-lamp" d="M13.5 22.5h2v3.2h-2Z" fill="var(--lamp)" />
    </svg>
  )
}

// A four pointed star with curved sides, centered on (x, y)
function sparkle(x: number, y: number, r: number) {
  return `M${x} ${y - r}Q${x} ${y} ${x + r} ${y}Q${x} ${y} ${x} ${y + r}Q${x} ${y} ${x - r} ${y}Q${x} ${y} ${x} ${y - r}Z`
}

// While something loads: three stars twinkling in turn
export function StarLoader({ className, label }: { className?: string; label?: string }) {
  const { t } = useTranslation()
  return (
    <div role="status" className={cn('flex flex-col items-center gap-2 text-sm text-muted-foreground', className)}>
      <svg viewBox="0 0 40 16" className="scenery-loading h-6 w-14" aria-hidden>
        {[6, 20, 34].map((x) => (
          <path key={x} className="scenery-sparkle" d={sparkle(x, 8, 5)} fill="var(--lamp)" />
        ))}
      </svg>
      <span>{label ?? t('common.loading')}</span>
    </div>
  )
}
