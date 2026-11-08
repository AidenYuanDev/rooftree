import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { formatTime, runs } from '~/lib/status'

// One measure over the day: a line with a faint area under it, in the theme's primary (indigo by day,
// lamp amber at night). The line breaks where samples are missing (a restart). Hovering shows the
// sample under the pointer: a crosshair, a dot and its time and value above the chart

const width = 600
const height = 120

export function StatusChart({
  title,
  at,
  values,
  interval,
  format,
}: {
  title: string
  at: number[]
  values: number[]
  interval: number
  format: (value: number) => string
}) {
  const { t, i18n } = useTranslation()
  const [hover, setHover] = useState<number | null>(null)
  const points = at.map((time, i) => ({ time, value: values[i] ?? 0 }))
  const first = points[0]
  const last = points[points.length - 1]
  if (!first || !last) return null

  const span = Math.max(last.time - first.time, interval)
  const max = Math.max(...values)
  const top = max * 1.15 || 1
  const x = (time: number) => ((time - first.time) / span) * width
  const y = (value: number) => height - (value / top) * height

  const lines = runs(at, interval).map(([from, to]) => {
    const run = points.slice(from, to).map((point) => [x(point.time), y(point.value)] as const)
    // A run of one sample is a dot's worth of line
    if (run.length === 1) run.push([run[0]![0] + 2, run[0]![1]])
    const path = run.map(([px, py]) => `${px.toFixed(1)},${py.toFixed(1)}`).join('L')
    return { line: `M${path}`, area: `M${run[0]![0].toFixed(1)},${height}L${path}L${run[run.length - 1]![0].toFixed(1)},${height}Z` }
  })

  const onPointer = (event: React.PointerEvent<HTMLDivElement>) => {
    const box = event.currentTarget.getBoundingClientRect()
    const time = first.time + ((event.clientX - box.left) / box.width) * span
    let nearest = 0
    points.forEach((point, i) => {
      if (Math.abs(point.time - time) < Math.abs(points[nearest]!.time - time)) nearest = i
    })
    setHover(nearest)
  }

  const shown = (hover !== null && points[hover]) || last
  const hovered = hover !== null ? points[hover] : undefined
  return (
    <figure className="rise-in rounded-2xl border bg-paper p-4 backdrop-blur-md">
      <figcaption className="mb-3 flex items-baseline justify-between gap-3">
        <span className="text-sm font-medium">{title}</span>
        <span className="text-sm text-muted-foreground tabular-nums">
          {hovered && <span className="mr-2">{formatTime(hovered.time, i18n.language)}</span>}
          <span className="font-medium text-foreground">{format(shown.value)}</span>
        </span>
      </figcaption>
      <div
        className="relative h-28 touch-none"
        onPointerMove={onPointer}
        onPointerDown={onPointer}
        onPointerLeave={() => setHover(null)}
        role="img"
        aria-label={`${title}: ${t('status.now')} ${format(last.value)}, ${format(Math.min(...values))} – ${format(max)}`}
      >
        <span className="absolute top-0 left-0 text-[10px] text-muted-foreground tabular-nums">{format(max)}</span>
        <svg viewBox={`0 0 ${width} ${height}`} preserveAspectRatio="none" className="size-full overflow-visible text-primary">
          <line x1={0} x2={width} y1={height} y2={height} className="stroke-border" vectorEffect="non-scaling-stroke" />
          <line
            x1={0}
            x2={width}
            y1={y(max)}
            y2={y(max)}
            className="stroke-border"
            strokeDasharray="3 4"
            vectorEffect="non-scaling-stroke"
          />
          {lines.map(({ line, area }) => (
            <g key={line}>
              <path d={area} fill="currentColor" fillOpacity={0.12} />
              <path d={line} fill="none" stroke="currentColor" strokeWidth={2} strokeLinejoin="round" vectorEffect="non-scaling-stroke" />
            </g>
          ))}
        </svg>
        {hovered && (
          <>
            <span
              className="pointer-events-none absolute inset-y-0 w-px bg-foreground/30"
              style={{ left: `${(x(hovered.time) / width) * 100}%` }}
            />
            <span
              className="pointer-events-none absolute size-2.5 -translate-x-1/2 -translate-y-1/2 rounded-full bg-primary ring-2 ring-background"
              style={{ left: `${(x(hovered.time) / width) * 100}%`, top: `${(y(hovered.value) / height) * 100}%` }}
            />
          </>
        )}
      </div>
      <div className="mt-1 flex justify-between text-[10px] text-muted-foreground tabular-nums">
        <span>{formatTime(first.time, i18n.language)}</span>
        <span>{t('status.now')}</span>
      </div>
    </figure>
  )
}
