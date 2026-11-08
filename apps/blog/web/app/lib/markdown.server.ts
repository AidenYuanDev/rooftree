// Markdown to HTML at build time (and in the dev server's loaders): GitHub flavored, headings with
// ids (rehype-slug, for the table of contents and links to a section), code blocks highlighted by Shiki
// with a light and a dark theme (switched by the .dark class, see app.css), and uploaded images with
// their size and blurred placeholder (thumbhash, computed when the admin uploaded them).
// .server: never shipped to the browser, Shiki alone is megabytes.
//
// Raw HTML in the markdown is not passed through (remark-rehype without allowDangerousHtml), so the
// output only has the elements markdown itself produces.

import rehypeShiki from '@shikijs/rehype'
import type { Element, Root } from 'hast'
import rehypeSlug from 'rehype-slug'
import rehypeStringify from 'rehype-stringify'
import remarkGfm from 'remark-gfm'
import remarkParse from 'remark-parse'
import remarkRehype from 'remark-rehype'
import { thumbHashToDataURL } from 'thumbhash'
import { unified } from 'unified'
import { visit } from 'unist-util-visit'
import { api } from './api'
import { UploadMeta } from './schemas'

// The uploads' placeholders, fetched once per build (the dev server: every time)
let uploads: { at: number; meta: Promise<UploadMeta['images']> } | null = null
const uploadsCacheTime = import.meta.env.DEV ? 0 : 60_000

function uploadMeta(): Promise<UploadMeta['images']> {
  if (!uploads || Date.now() - uploads.at >= uploadsCacheTime) {
    const meta = api('/api/uploads/meta', UploadMeta).then(
      (result) => result.images,
      // Without them images just load without a placeholder
      () => ({}),
    )
    uploads = { at: Date.now(), meta }
  }
  return uploads.meta
}

// The images come with each file (renderMarkdown), the processor is made once
function rehypeUploads() {
  return (tree: Root, file: { data: object }) => {
    const images = (file.data as { images?: UploadMeta['images'] }).images ?? {}
    visit(tree, 'element', (node: Element) => {
      if (node.tagName !== 'img') return
      const src = typeof node.properties.src === 'string' ? node.properties.src : ''
      node.properties.loading = 'lazy'
      node.properties.decoding = 'async'
      const image = src.startsWith('/uploads/') ? images[src.slice('/uploads/'.length)] : undefined
      if (!image) return

      node.properties.width = image.width
      node.properties.height = image.height
      if (image.thumbhash) {
        const hash = Uint8Array.from(atob(image.thumbhash), (c) => c.charCodeAt(0))
        node.properties.style = `background:center/cover no-repeat url(${thumbHashToDataURL(hash)})`
      }
    })
  }
}

const processor = unified()
  .use(remarkParse)
  .use(remarkGfm)
  .use(remarkRehype)
  .use(rehypeSlug)
  .use(rehypeUploads)
  .use(rehypeShiki, {
    themes: { light: 'github-light', dark: 'github-dark' },
    defaultColor: false,
    // An unknown language stays plain text instead of failing the build
    fallbackLanguage: 'text',
  })
  .use(rehypeStringify)

export async function renderMarkdown(markdown: string): Promise<string> {
  return String(await processor.process({ value: markdown, data: { images: await uploadMeta() } }))
}
