import { useTranslation } from 'react-i18next'
import type { Route } from './+types/status'
import { PageHeader } from '~/components/page-header'
import { RequireUser } from '~/components/require-user'
import { StatusChart } from '~/components/status-chart'
import { Skeleton } from '~/components/ui/skeleton'
import { useLang } from '~/lib/lang'
import { privateMeta } from '~/lib/page'
import { useServerStatus } from '~/lib/queries'
import type { ServerStatus, StatusMeasure } from '~/lib/schemas'
import { formatTime, formatUptime } from '~/lib/status'

// 看云: how the server under the site is doing, for logged in readers. The page is a prerendered shell,
// the numbers come from /api/status once the reader is known (a login link otherwise)

export const meta = ({ params }: Route.MetaArgs) => privateMeta(params.lang, 'status.title')

const number = (value: number) => value.toLocaleString()
const percent = (value: number) => `${value}%`
const microseconds = (value: number) => (value < 1000 ? `${value} µs` : `${(value / 1000).toFixed(1)} ms`)

function Tile({ label, value, detail }: { label: string; value: string; detail?: string }) {
  return (
    <div className="rise-in rounded-2xl border bg-paper p-4 backdrop-blur-md">
      <p className="text-xs text-muted-foreground">{label}</p>
      <p className="mt-1 font-heading text-2xl font-bold tabular-nums">{value}</p>
      {detail && <p className="mt-0.5 text-xs text-muted-foreground">{detail}</p>}
    </div>
  )
}

function Numbers({ status }: { status: ServerStatus }) {
  const { t } = useTranslation()
  const lang = useLang()
  const { now, series, interval } = status

  const charts: { measure: StatusMeasure; title: string; format: (value: number) => string }[] = [
    { measure: 'readers', title: t('status.readers'), format: number },
    { measure: 'requests', title: t('status.requests'), format: number },
    { measure: 'updateAvgUs', title: t('status.update'), format: microseconds },
    { measure: 'cpuPercent', title: t('status.cpu'), format: percent },
    { measure: 'memoryMiB', title: t('status.memory'), format: (value) => `${value} MiB` },
    { measure: 'load', title: t('status.load'), format: (value) => value.toFixed(1) },
  ]

  return (
    <>
      <div className="grid grid-cols-2 gap-3 md:grid-cols-4">
        <Tile label={t('status.uptime')} value={formatUptime(now.uptime, lang)} />
        <Tile label={t('status.readers')} value={number(now.readers)} detail={t('status.pages', { count: now.pages })} />
        <Tile label={t('status.requests')} value={number(now.requests)} />
        <Tile
          label={t('status.update')}
          value={microseconds(now.updateAvgUs)}
          detail={t('status.updateMax', { value: microseconds(now.updateMaxUs) })}
        />
        <Tile label={t('status.cpu')} value={percent(now.cpuPercent)} detail={t('status.cores', { count: now.cores })} />
        <Tile label={t('status.memory')} value={`${now.memoryMiB} MiB`} detail={t('status.hostMemory', { value: now.hostMemoryPercent })} />
        <Tile label={t('status.load')} value={now.load.toFixed(1)} detail={t('status.cores', { count: now.cores })} />
        <Tile label={t('status.disk')} value={percent(now.diskFreePercent)} />
        <Tile
          label={t('status.chatMessages')}
          value={number(now.chatMessagesTotal ?? 0)}
          detail={t('status.chatChannels', { count: now.chatChannelsCount ?? 0 })}
        />
      </div>

      <h2 className="mt-10 mb-4 font-heading text-xl font-bold">{t('status.day')}</h2>
      {series.at.length === 0 ? (
        <p className="text-sm text-muted-foreground">{t('status.empty')}</p>
      ) : (
        <div className="grid gap-4 md:grid-cols-2">
          {charts.map(({ measure, title, format }) => (
            <StatusChart key={measure} title={title} at={series.at} values={series[measure]} interval={interval} format={format} />
          ))}
        </div>
      )}
      <p className="mt-6 text-xs text-muted-foreground">{t('status.updated', { time: formatTime(now.at, lang) })}</p>
    </>
  )
}

function Status() {
  const { t } = useTranslation()
  const status = useServerStatus()
  if (status.data) return <Numbers status={status.data} />
  if (status.isError) return <p className="py-16 text-center text-muted-foreground">{t('status.error')}</p>
  return <Skeleton className="h-48 w-full" />
}

export default function StatusPage() {
  const { t } = useTranslation()
  return (
    <div className="mx-auto max-w-5xl">
      <PageHeader title={t('status.title')} description={t('status.description')} />
      <RequireUser admin prompt={t('status.loginRequired')}>
        {() => <Status />}
      </RequireUser>
    </div>
  )
}
