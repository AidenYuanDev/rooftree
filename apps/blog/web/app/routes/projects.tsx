import { ExternalLinkIcon, FolderGit2Icon } from 'lucide-react'
import { siGithub } from 'simple-icons'
import { useTranslation } from 'react-i18next'
import type { Route } from './+types/projects'
import { PageHeader } from '~/components/page-header'
import { getI18n } from '~/i18n'
import { fetchProjects, fetchSiteCached } from '~/lib/content'
import { useLang } from '~/lib/lang'
import { pageMeta } from '~/lib/meta'
import { requireLang } from '~/lib/route'

async function load(lang: string | undefined) {
  const valid = requireLang(lang)
  const [projects, site] = await Promise.all([fetchProjects(), fetchSiteCached(valid)])
  return { projects, siteName: site.name }
}

export async function loader({ params }: Route.LoaderArgs) {
  return load(params.lang)
}

export async function clientLoader({ params, serverLoader }: Route.ClientLoaderArgs) {
  try {
    return await serverLoader()
  } catch {
    return load(params.lang)
  }
}
// With hydrate and no HydrateFallback, the prerender renders the page itself and hydration reuses its data
clientLoader.hydrate = true as const

export function meta({ params, loaderData }: Route.MetaArgs) {
  const lang = params.lang === 'en' ? 'en' : 'zh'
  return pageMeta({ lang, title: `${getI18n(lang).t('nav.projects')} · ${loaderData?.siteName ?? ''}`, path: '/projects' })
}

export default function Projects({ loaderData }: Route.ComponentProps) {
  const { t } = useTranslation()
  const lang = useLang()
  return (
    <div>
      <PageHeader title={t('nav.projects')} description={t('projects.description')} />
      {loaderData.projects.length === 0 && <p className="py-12 text-center text-muted-foreground">{t('projects.empty')}</p>}
      <ul className="grid gap-4 sm:grid-cols-2 lg:grid-cols-3">
        {loaderData.projects.map((project, i) => {
          const description =
            (lang === 'en' ? project.descriptionEn : project.descriptionZh) || project.descriptionZh || project.descriptionEn
          return (
            <li
              key={project.id}
              className="rise-in flex flex-col rounded-2xl border bg-card/60 p-5 shadow-sm backdrop-blur-md transition hover:-translate-y-0.5 hover:border-primary/40"
              style={{ '--rise-index': Math.min(i, 8) } as React.CSSProperties}
            >
              <div className="flex items-center gap-3">
                {project.icon ? (
                  <img src={project.icon} alt="" className="size-10 rounded-lg object-cover" />
                ) : (
                  <span className="flex size-10 items-center justify-center rounded-lg bg-primary/15 text-primary">
                    <FolderGit2Icon className="size-5" />
                  </span>
                )}
                <h2 className="font-heading text-lg font-bold">{project.name}</h2>
              </div>
              {description && <p className="mt-3 flex-1 text-sm text-muted-foreground">{description}</p>}
              <div className="mt-4 flex gap-3 text-sm">
                {project.url && (
                  <a href={project.url} target="_blank" rel="noopener" className="flex items-center gap-1 text-primary hover:underline">
                    <ExternalLinkIcon className="size-3.5" />
                    {t('projects.visit')}
                  </a>
                )}
                {project.repoUrl && (
                  <a
                    href={project.repoUrl}
                    target="_blank"
                    rel="noopener"
                    className="flex items-center gap-1 text-muted-foreground hover:text-foreground"
                  >
                    <svg viewBox="0 0 24 24" className="size-3.5" fill="currentColor" aria-hidden>
                      <path d={siGithub.path} />
                    </svg>
                    {t('projects.source')}
                  </a>
                )}
              </div>
            </li>
          )
        })}
      </ul>
    </div>
  )
}
