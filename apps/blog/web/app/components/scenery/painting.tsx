import { useEffect, useId, useRef } from 'react'
import { cn } from '~/lib/utils'

// Behind every page: an oil painting of a cottage under a tree on a hillside. By day in violet with the
// sun behind the tree, at night under the stars with the windows lit. Wide windows get the landscape
// painting, narrow ones the portrait (public/scenery/, chosen in scenery.css, so only one is fetched).
//
// Over it, what moves: at night the painting's own stars twinkle, a star falls now and then and the
// lamps behind the windows flicker; by day the light behind the tree and on the grass breathes. Drawn in
// the painting's pixels, in an SVG that covers the window the way the background does (slice = cover),
// so it stays on the painting at any size. The motions in scenery.css; none for "reduce motion".

interface Spot {
  x: number
  y: number
  r: number
}

interface Scene {
  width: number
  height: number
  // The stars of the night painting, found by their brightness; size from the area they cover
  stars: { x: number; y: number; area: number }[]
  windows: (Spot & { w: number; h: number })[]
  // A falling star: where it starts, crossing the open sky down to the left
  meteor: { x: number; y: number; length: number }
  // By day: the sun behind the tree, the light on the grass
  sun: Spot
  sunlight: Spot & { rx: number }
}

const scenes: Record<'wide' | 'tall', Scene> = {
  wide: {
    width: 1024,
    height: 572,
    stars: [
      { x: 198, y: 161.5, area: 22 },
      { x: 783, y: 81, area: 13 },
      { x: 47, y: 95, area: 10 },
      { x: 957, y: 144, area: 10 },
      { x: 208, y: 199, area: 9 },
      { x: 699, y: 261, area: 8 },
      { x: 159, y: 48, area: 6 },
      { x: 900, y: 167.5, area: 6 },
      { x: 837, y: 209, area: 6 },
      { x: 889, y: 217, area: 5 },
      { x: 776.5, y: 56.5, area: 4 },
      { x: 653.5, y: 58.5, area: 4 },
      { x: 82.5, y: 62.5, area: 4 },
      { x: 797, y: 176, area: 4 },
      { x: 63, y: 188, area: 4 },
      { x: 923.5, y: 194.5, area: 4 },
      { x: 910.5, y: 207.5, area: 4 },
      { x: 608, y: 32, area: 3 },
      { x: 641, y: 87, area: 3 },
      { x: 22, y: 147, area: 3 },
      { x: 928, y: 153, area: 3 },
      { x: 689, y: 161, area: 3 },
      { x: 676, y: 175, area: 3 },
      { x: 978, y: 219, area: 3 },
      { x: 984, y: 253, area: 3 },
      { x: 866, y: 33.5, area: 2 },
      { x: 293.5, y: 45, area: 2 },
      { x: 203, y: 102.5, area: 2 },
      { x: 843, y: 145.5, area: 2 },
      { x: 737.5, y: 216, area: 2 },
      { x: 1014.5, y: 240, area: 2 },
    ],
    windows: [
      { x: 442, y: 362, w: 8, h: 25, r: 46 },
      { x: 321, y: 336, w: 3, h: 7, r: 22 },
    ],
    meteor: { x: 960, y: 40, length: 90 },
    sun: { x: 535, y: 300, r: 120 },
    sunlight: { x: 462, y: 452, r: 34, rx: 60 },
  },
  tall: {
    width: 572,
    height: 1024,
    stars: [
      { x: 98, y: 236, area: 6 },
      { x: 557, y: 291, area: 4 },
      { x: 413, y: 199, area: 3 },
      { x: 514, y: 221, area: 3 },
      { x: 524, y: 255, area: 3 },
      { x: 504, y: 288, area: 3 },
      { x: 557, y: 333, area: 3 },
      { x: 558, y: 427, area: 3 },
      { x: 75, y: 249.5, area: 2 },
      { x: 72.5, y: 327, area: 2 },
      { x: 374, y: 178, area: 1.5 },
      { x: 462, y: 197, area: 1.5 },
      { x: 42, y: 208, area: 1.5 },
      { x: 37, y: 351, area: 1.5 },
    ],
    windows: [
      { x: 294, y: 649, w: 11, h: 34, r: 60 },
      { x: 133, y: 610, w: 2, h: 18, r: 26 },
    ],
    meteor: { x: 520, y: 70, length: 80 },
    sun: { x: 415, y: 570, r: 110 },
    sunlight: { x: 322, y: 762, r: 40, rx: 64 },
  },
}

// A four pointed star with curved sides, centered on (x, y)
function sparkle(x: number, y: number, r: number) {
  return `M${x} ${y - r}Q${x} ${y} ${x + r} ${y}Q${x} ${y} ${x} ${y + r}Q${x} ${y} ${x - r} ${y}Q${x} ${y} ${x} ${y - r}Z`
}

function Overlay({ scene, className }: { scene: Scene; className: string }) {
  const id = useId()
  return (
    <svg
      viewBox={`0 0 ${scene.width} ${scene.height}`}
      preserveAspectRatio="xMidYMid slice"
      className={cn('absolute inset-0 size-full', className)}
    >
      <defs>
        {/* Not "transparent": that is transparent black, and the glow would go grey on its way out */}
        <radialGradient id={`${id}-lamp`}>
          <stop offset={0} stopColor="var(--lamp)" stopOpacity={0.85} />
          <stop offset={0.25} stopColor="var(--lamp)" stopOpacity={0.35} />
          <stop offset={1} stopColor="var(--lamp)" stopOpacity={0} />
        </radialGradient>
        <radialGradient id={`${id}-sun`}>
          <stop offset={0} stopColor="var(--sun)" stopOpacity={0.7} />
          <stop offset={1} stopColor="var(--sun)" stopOpacity={0} />
        </radialGradient>
        <linearGradient id={`${id}-meteor`}>
          <stop offset={0} stopColor="var(--star)" />
          <stop offset={1} stopColor="var(--star)" stopOpacity={0} />
        </linearGradient>
      </defs>

      <g className="scenery-night-only">
        <g fill="var(--star)">
          {scene.stars.map((star, i) => {
            const size = Math.sqrt(star.area)
            return (
              <g
                key={`${star.x},${star.y}`}
                className="scenery-star"
                // Each on its own beat
                style={{ animationDuration: `${2.6 + ((i * 0.7) % 2.9)}s`, animationDelay: `${i * -1.3}s` }}
              >
                <circle cx={star.x} cy={star.y} r={size * 1.6} opacity={0.18} />
                {star.area >= 9 ? <path d={sparkle(star.x, star.y, size * 1.3)} /> : <circle cx={star.x} cy={star.y} r={size * 0.7} />}
              </g>
            )
          })}
        </g>
        <g className="scenery-meteor" style={{ '--meteor-length': `${scene.meteor.length}px` } as React.CSSProperties}>
          {/* Down to the left: the head at (x, y), the tail behind it up to the right */}
          <rect
            x={scene.meteor.x}
            y={scene.meteor.y}
            width={scene.meteor.length}
            height={1.2}
            rx={0.6}
            fill={`url(#${id}-meteor)`}
            transform={`rotate(-24 ${scene.meteor.x} ${scene.meteor.y})`}
          />
        </g>
        {scene.windows.map((window, i) => (
          <g key={window.x} className="scenery-lamp" style={{ '--lamp-index': i } as React.CSSProperties}>
            <ellipse cx={window.x} cy={window.y + window.h * 0.15} rx={window.r} ry={window.r * 0.85} fill={`url(#${id}-lamp)`} />
            <rect
              x={window.x - window.w / 2}
              y={window.y - window.h / 2}
              width={window.w}
              height={window.h}
              rx={1}
              fill="var(--lamp)"
              opacity={0.35}
            />
          </g>
        ))}
      </g>

      <g className="scenery-day-only">
        <circle className="scenery-sun" cx={scene.sun.x} cy={scene.sun.y} r={scene.sun.r} fill={`url(#${id}-sun)`} />
        <ellipse
          className="scenery-sunlight"
          cx={scene.sunlight.x}
          cy={scene.sunlight.y}
          rx={scene.sunlight.rx}
          ry={scene.sunlight.r}
          fill={`url(#${id}-sun)`}
        />
      </g>
    </svg>
  )
}

// The address scenery.css picked for this window, from its url('…') in --day or --night
function paintingSource(scenery: Element, theme: 'day' | 'night') {
  return /url\(\s*['"]?([^'")]+)/.exec(getComputedStyle(scenery).getPropertyValue(`--${theme}`))?.[1]
}

export function Painting() {
  const ref = useRef<HTMLDivElement>(null)

  // The motions show once the painting under them is there (at once when the browser has it)
  useEffect(() => {
    const scenery = ref.current
    const src = scenery && paintingSource(scenery, document.documentElement.classList.contains('dark') ? 'night' : 'day')
    if (!scenery || !src) return
    const image = new Image()
    const ready = () => scenery.setAttribute('data-ready', '')
    image.addEventListener('load', ready)
    image.addEventListener('error', ready)
    image.src = src
    if (image.complete) ready()
    return () => image.removeEventListener('load', ready)
  }, [])

  // Fetch the other theme's painting once the page is quiet, so switching shows it at once
  useEffect(() => {
    const timer = window.setTimeout(() => {
      const scenery = ref.current
      const src = scenery && paintingSource(scenery, document.documentElement.classList.contains('dark') ? 'day' : 'night')
      if (src) new Image().src = src
    }, 3000)
    return () => window.clearTimeout(timer)
  }, [])

  return (
    <div ref={ref} aria-hidden className="scenery pointer-events-none fixed inset-0 -z-10 overflow-hidden">
      <div className="scenery-sketch absolute inset-0" />
      <div className="scenery-painting absolute inset-0" />
      <Overlay scene={scenes.wide} className="scenery-wide" />
      <Overlay scene={scenes.tall} className="scenery-tall" />
      {/* Between the painting and the page: enough to read over it */}
      <div className="scenery-veil absolute inset-0" />
    </div>
  )
}
