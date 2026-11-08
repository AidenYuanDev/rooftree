import { cn } from '~/lib/utils'

// The title of a list page, in the heading font, with a line under it
export function PageHeader({
  title,
  description,
  actions,
  className,
}: {
  title: string
  description?: string
  actions?: React.ReactNode
  className?: string
}) {
  return (
    <header className={cn('rise-in mb-10', className)}>
      <div className="flex flex-wrap items-center justify-between gap-3">
        <h1 className="font-heading text-3xl font-bold md:text-4xl">{title}</h1>
        {actions && <div className="flex flex-wrap items-center gap-2">{actions}</div>}
      </div>
      {description && <p className="mt-3 text-muted-foreground">{description}</p>}
    </header>
  )
}
