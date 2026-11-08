import { useEffect, useState } from 'react'
import Lightbox from 'yet-another-react-lightbox'
import 'yet-another-react-lightbox/styles.css'

// Click an image of a post to see it large; the post's other images are a swipe away. Listens on the
// container, so it works on prerendered HTML as it is
export function ImageViewer({ container, contentKey }: { container: React.RefObject<HTMLElement | null>; contentKey: unknown }) {
  const [slides, setSlides] = useState<{ src: string; alt: string }[]>([])
  const [index, setIndex] = useState(-1)

  useEffect(() => {
    const root = container.current
    if (!root) return
    const onClick = (event: MouseEvent) => {
      const image = event.target instanceof HTMLImageElement ? event.target : null
      if (!image || image.closest('a')) return
      const images = [...root.querySelectorAll<HTMLImageElement>('.prose img')]
      setSlides(images.map((img) => ({ src: img.currentSrc || img.src, alt: img.alt })))
      setIndex(images.indexOf(image))
    }
    root.addEventListener('click', onClick)
    return () => root.removeEventListener('click', onClick)
  }, [container, contentKey])

  return (
    <Lightbox
      open={index >= 0}
      index={Math.max(index, 0)}
      close={() => setIndex(-1)}
      slides={slides}
      controller={{ closeOnBackdropClick: true }}
      styles={{ container: { backgroundColor: 'oklch(0.12 0.03 270 / 92%)' } }}
    />
  )
}
