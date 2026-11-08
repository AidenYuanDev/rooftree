import { motion, type MotionValue, useMotionValue, useSpring, useTransform } from 'motion/react'
import { useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { cn } from '~/lib/utils'

// The table of contents beside a post, made from its rendered headings (h2, h3 with ids), so it
// works the same for prerendered HTML and posts rendered in the browser. The current section is
// marked while reading; below, how far through the post the reader is.

interface Heading {
  id: string
  text: string
  level: number
}

function useHeadings(container: React.RefObject<HTMLElement | null>, key: unknown) {
  const [headings, setHeadings] = useState<Heading[]>([])
  useEffect(() => {
    const root = container.current
    if (!root) return
    const read = () =>
      setHeadings(
        [...root.querySelectorAll<HTMLHeadingElement>('h2[id], h3[id]')].map((heading) => ({
          id: heading.id,
          text: heading.textContent ?? '',
          level: heading.tagName === 'H2' ? 2 : 3,
        })),
      )
    read()
    // A post rendered in the browser (lazy Markdown) appears after this effect
    const observer = new MutationObserver(read)
    observer.observe(root, { childList: true, subtree: true })
    return () => observer.disconnect()
  }, [container, key])
  return headings
}

// The last heading above the upper third of the window
function useActive(headings: Heading[]) {
  const [active, setActive] = useState<string | null>(null)
  useEffect(() => {
    if (headings.length === 0) return
    const update = () => {
      let current: string | null = null
      for (const heading of headings) {
        const element = document.getElementById(heading.id)
        if (element && element.getBoundingClientRect().top < window.innerHeight / 3) current = heading.id
      }
      setActive(current)
    }
    update()
    window.addEventListener('scroll', update, { passive: true })
    return () => window.removeEventListener('scroll', update)
  }, [headings])
  return active
}

// How much of the post has come into view, 0 to 1: a short post shown whole is read to the end
function useReadProgress(container: React.RefObject<HTMLElement | null>): MotionValue<number> {
  const progress = useMotionValue(0)
  useEffect(() => {
    const update = () => {
      const box = container.current?.getBoundingClientRect()
      if (!box || box.height === 0) return
      progress.set(Math.min(Math.max((window.innerHeight - box.top) / box.height, 0), 1))
    }
    update()
    const observer = new ResizeObserver(update)
    if (container.current) observer.observe(container.current)
    window.addEventListener('scroll', update, { passive: true })
    window.addEventListener('resize', update)
    return () => {
      observer.disconnect()
      window.removeEventListener('scroll', update)
      window.removeEventListener('resize', update)
    }
  }, [container, progress])
  return progress
}

export function TableOfContents({ container, contentKey }: { container: React.RefObject<HTMLElement | null>; contentKey: unknown }) {
  const { t } = useTranslation()
  const headings = useHeadings(container, contentKey)
  const active = useActive(headings)
  const read = useReadProgress(container)
  const progress = useSpring(read, { stiffness: 200, damping: 30 })
  const percent = useTransform(read, (value) => `${Math.round(value * 100)}%`)

  return (
    <nav aria-label={t('posts.toc')} className="sticky top-24 hidden max-h-[calc(100dvh-8rem)] flex-col xl:flex">
      {headings.length > 0 && (
        <>
          <h2 className="mb-3 font-heading text-sm font-bold text-muted-foreground">{t('posts.toc')}</h2>
          <ol className="min-h-0 space-y-1 overflow-y-auto border-l text-sm">
            {headings.map((heading) => (
              <li key={heading.id}>
                <a
                  href={`#${heading.id}`}
                  className={cn(
                    '-ml-px block border-l-2 py-1 pr-2 transition-colors',
                    heading.level === 3 ? 'pl-6' : 'pl-3',
                    active === heading.id
                      ? 'border-primary font-medium text-primary'
                      : 'border-transparent text-muted-foreground hover:text-foreground',
                  )}
                >
                  {heading.text}
                </a>
              </li>
            ))}
          </ol>
        </>
      )}
      <div className="mt-5 flex items-center gap-3 text-xs text-muted-foreground">
        <div className="h-1 flex-1 overflow-hidden rounded-full bg-muted">
          <motion.div className="h-full origin-left rounded-full bg-primary" style={{ scaleX: progress }} />
        </div>
        <motion.span className="w-9 text-right tabular-nums">{percent}</motion.span>
      </div>
      <button
        type="button"
        onClick={() => window.scrollTo({ top: 0, behavior: 'smooth' })}
        className="mt-3 self-start text-xs text-muted-foreground hover:text-foreground"
      >
        ↑ {t('posts.backToTop')}
      </button>
    </nav>
  )
}

// A thin bar under the header: how far through the post (small screens, no table of contents there)
export function ReadingProgress({ container }: { container: React.RefObject<HTMLElement | null> }) {
  const scaleX = useSpring(useReadProgress(container), { stiffness: 200, damping: 30 })
  return <motion.div aria-hidden className="fixed inset-x-0 top-16 z-40 h-0.5 origin-left bg-primary xl:hidden" style={{ scaleX }} />
}
