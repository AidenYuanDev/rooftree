import ReactMarkdown from 'react-markdown'
import rehypeSlug from 'rehype-slug'
import remarkGfm from 'remark-gfm'
import { cn } from '~/lib/utils'

// Markdown rendered in the browser: the editor's preview, and posts newer than the last build. No raw
// HTML (react-markdown escapes it); published pages use the build's highlighted HTML instead
export function Markdown({ source, className }: { source: string; className?: string }) {
  return (
    <div className={cn('prose max-w-none prose-night', className)}>
      <ReactMarkdown remarkPlugins={[remarkGfm]} rehypePlugins={[rehypeSlug]}>
        {source}
      </ReactMarkdown>
    </div>
  )
}
