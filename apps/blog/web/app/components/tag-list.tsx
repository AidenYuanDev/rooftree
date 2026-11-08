import { Link } from 'react-router'
import { Badge } from '~/components/ui/badge'
import { localePath, useLang } from '~/lib/lang'
import type { Tag } from '~/lib/schemas'
import { cn } from '~/lib/utils'

export function TagList({ tags, current }: { tags: Tag[]; current?: string }) {
  const lang = useLang()
  const used = tags.filter((tag) => tag.postCount > 0)
  return (
    <div className="flex flex-wrap gap-1.5">
      {used.map((tag) => (
        <Badge key={tag.slug} variant={tag.slug === current ? 'default' : 'outline'} asChild>
          <Link to={localePath(lang, `/tags/${tag.slug}`)} className={cn(tag.slug === current && 'pointer-events-none')}>
            {lang === 'en' ? tag.nameEn : tag.nameZh}
            <span className="ml-1 text-xs opacity-60">{tag.postCount}</span>
          </Link>
        </Badge>
      ))}
    </div>
  )
}
