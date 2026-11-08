import { renderToStaticMarkup } from 'react-dom/server'
import ReactMarkdown from 'react-markdown'
import rehypeSlug from 'rehype-slug'
import remarkGfm from 'remark-gfm'

// Markdown as the browser renders it (components/markdown.tsx), as HTML: for a post newer than
// the build, shown as the build's HTML is, so what goes into its text (components/passages.tsx) is the
// page's, not React's. Loaded when needed
export function markdownHtml(source: string): string {
  return renderToStaticMarkup(
    <ReactMarkdown remarkPlugins={[remarkGfm]} rehypePlugins={[rehypeSlug]}>
      {source}
    </ReactMarkdown>,
  )
}
