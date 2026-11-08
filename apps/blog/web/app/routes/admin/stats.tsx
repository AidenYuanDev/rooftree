import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { FormError } from '~/components/form'
import { Button } from '~/components/ui/button'
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '~/components/ui/card'
import { Skeleton } from '~/components/ui/skeleton'
import type { AdminStats } from '~/lib/schemas'
import { useAdminStats } from '~/lib/queries'

const ranges = [7, 30, 90] as const

// Reads per day as bars, visitors the darker part of each
function Chart({ days }: { days: AdminStats['days'] }) {
  const { t } = useTranslation()
  const top = Math.max(1, ...days.map((day) => day.views))
  const width = 12
  const height = 120
  return (
    <figure className="grid gap-1">
      <svg
        viewBox={`0 0 ${days.length * width} ${height}`}
        preserveAspectRatio="none"
        className="h-40 w-full"
        role="img"
        aria-label={t('admin.statsViews')}
      >
        {days.map((day, index) => {
          const views = (day.views / top) * height
          const visitors = (day.visitors / top) * height
          return (
            <g key={day.day}>
              <title>{`${day.day} · ${t('admin.statsViews')} ${day.views} · ${t('admin.statsVisitors')} ${day.visitors}`}</title>
              <rect x={index * width + 1} y={height - views} width={width - 2} height={views} className="fill-primary/35" />
              <rect x={index * width + 1} y={height - visitors} width={width - 2} height={visitors} className="fill-primary" />
            </g>
          )
        })}
      </svg>
      <figcaption className="flex justify-between text-xs text-muted-foreground">
        <span>{days[0]?.day}</span>
        <span className="flex items-center gap-3">
          <span className="flex items-center gap-1">
            <span className="size-2.5 rounded-sm bg-primary/35" />
            {t('admin.statsViews')}
          </span>
          <span className="flex items-center gap-1">
            <span className="size-2.5 rounded-sm bg-primary" />
            {t('admin.statsVisitors')}
          </span>
        </span>
        <span>{days.at(-1)?.day}</span>
      </figcaption>
    </figure>
  )
}

function Figure({ label, views, visitors }: { label: string; views: number; visitors: number }) {
  const { t } = useTranslation()
  return (
    <Card>
      <CardHeader>
        <CardDescription>{label}</CardDescription>
        <CardTitle className="text-2xl tabular-nums">{views.toLocaleString()}</CardTitle>
      </CardHeader>
      <CardContent className="text-sm text-muted-foreground">
        {t('admin.statsVisitors')} {visitors.toLocaleString()}
      </CardContent>
    </Card>
  )
}

// What readers read and where they came from, per day, for the last days
export default function AdminStatsPage() {
  const { t } = useTranslation()
  const [days, setDays] = useState<number>(30)
  const stats = useAdminStats(days)

  return (
    <div className="grid gap-6">
      <div className="flex flex-wrap items-end justify-between gap-3">
        <div>
          <h1 className="text-xl font-semibold">{t('admin.stats')}</h1>
          <p className="text-sm text-muted-foreground">{t('admin.statsHint')}</p>
        </div>
        <div className="flex gap-1">
          {ranges.map((range) => (
            <Button key={range} size="sm" variant={range === days ? 'secondary' : 'ghost'} onClick={() => setDays(range)}>
              {t('admin.statsDays', { count: range })}
            </Button>
          ))}
        </div>
      </div>

      {stats.isPending ? (
        <Skeleton className="h-96 w-full" />
      ) : !stats.data ? (
        <FormError error={stats.error} />
      ) : (
        <Stats stats={stats.data} />
      )}
    </div>
  )
}

function Stats({ stats }: { stats: AdminStats }) {
  const { t } = useTranslation()
  const today = stats.days.at(-1) ?? { views: 0, visitors: 0 }
  const views = stats.days.reduce((sum, day) => sum + day.views, 0)
  const visitors = stats.days.reduce((sum, day) => sum + day.visitors, 0)
  const referred = stats.referrers.reduce((sum, referrer) => sum + referrer.views, 0)

  return (
    <>
      <div className="grid gap-4 sm:grid-cols-2">
        <Figure label={t('admin.statsToday')} views={today.views} visitors={today.visitors} />
        <Figure label={t('admin.statsTotal')} views={views} visitors={visitors} />
      </div>
      <Chart days={stats.days} />

      {views === 0 ? (
        <p className="py-10 text-center text-muted-foreground">{t('admin.statsNone')}</p>
      ) : (
        <div className="grid gap-6 lg:grid-cols-[2fr_1fr]">
          <section className="grid content-start gap-2">
            <h2 className="font-semibold">{t('admin.statsPages')}</h2>
            <table className="w-full text-sm">
              <thead className="text-left text-xs text-muted-foreground">
                <tr className="border-b">
                  <th className="py-1.5 font-normal">{t('admin.statsPage')}</th>
                  <th className="py-1.5 text-right font-normal">{t('admin.statsViews')}</th>
                  <th className="py-1.5 text-right font-normal">{t('admin.statsVisitors')}</th>
                </tr>
              </thead>
              <tbody className="divide-y">
                {stats.pages.map((page) => (
                  <tr key={page.path}>
                    <td className="max-w-0 truncate py-1.5">
                      <a href={page.path} target="_blank" rel="noreferrer" className="hover:underline">
                        {decodeURIComponent(page.path)}
                      </a>
                    </td>
                    <td className="py-1.5 text-right tabular-nums">{page.views}</td>
                    <td className="py-1.5 text-right tabular-nums">{page.visitors}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </section>
          <section className="grid content-start gap-2">
            <h2 className="font-semibold">{t('admin.statsReferrers')}</h2>
            <table className="w-full text-sm">
              <thead className="text-left text-xs text-muted-foreground">
                <tr className="border-b">
                  <th className="py-1.5 font-normal">{t('admin.statsSite')}</th>
                  <th className="py-1.5 text-right font-normal">{t('admin.statsViews')}</th>
                </tr>
              </thead>
              <tbody className="divide-y">
                {stats.referrers.map((referrer) => (
                  <tr key={referrer.host}>
                    <td className="max-w-0 truncate py-1.5">{referrer.host}</td>
                    <td className="py-1.5 text-right tabular-nums">{referrer.views}</td>
                  </tr>
                ))}
              </tbody>
            </table>
            {views > referred && <p className="text-xs text-muted-foreground">{t('admin.statsDirect')}</p>}
          </section>
        </div>
      )}
    </>
  )
}
