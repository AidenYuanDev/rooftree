import { useCallback, useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { errorMessage } from '~/i18n'
import { formatBytes, uploadToDrive } from './drive'
import { sendThumbnail } from './thumbnail'
import { useLang } from './lang'
import type { DriveUsage } from './schemas'

export interface Upload {
  key: number
  file: File
  folderId: number | null
  sent: number
  state: 'waiting' | 'uploading' | 'done' | 'failed' | 'cancelled'
  error?: string
}

let nextKey = 1

// Uploads one at a time, in the order given, into the folder each was dropped on; they go on while
// the reader moves between folders. onUploaded: after each file, to show it
export function useUploads(usage: DriveUsage | undefined, onUploaded: () => void, owner?: number) {
  const { t, i18n } = useTranslation()
  const lang = useLang()
  const [uploads, setUploads] = useState<Upload[]>([])
  const controllers = useRef(new Map<number, AbortController>())
  const running = useRef(false)

  const update = (key: number, change: Partial<Upload>) =>
    setUploads((list) => list.map((upload) => (upload.key === key ? { ...upload, ...change } : upload)))

  const add = useCallback(
    (files: File[], folderId: number | null) =>
      setUploads((list) => [
        ...list,
        ...files.map((file): Upload => {
          const tooBig = usage && file.size > usage.maxFileSize
          return {
            key: nextKey++,
            file,
            folderId,
            sent: 0,
            state: tooBig ? 'failed' : 'waiting',
            error: tooBig ? t('drive.tooBig', { size: formatBytes(usage.maxFileSize, lang) }) : undefined,
          }
        }),
      ]),
    [usage, t, lang],
  )

  useEffect(() => {
    const next = uploads.find((upload) => upload.state === 'waiting')
    if (running.current || !next || !usage) return
    running.current = true
    const controller = new AbortController()
    controllers.current.set(next.key, controller)
    update(next.key, { state: 'uploading', sent: 0, error: undefined })
    uploadToDrive(next.file, {
      folderId: next.folderId,
      owner,
      chunkSize: usage.chunkSize,
      signal: controller.signal,
      onProgress: (sent) => update(next.key, { sent }),
    })
      .then((uploaded) => {
        update(next.key, { state: 'done', sent: next.file.size })
        onUploaded()
        // Its small picture meanwhile, shown once it is there
        void sendThumbnail(uploaded.id, next.file, uploaded.mime, owner).then((sent) => sent && onUploaded())
      })
      .catch((error: unknown) => {
        if (controller.signal.aborted) update(next.key, { state: 'cancelled' })
        else update(next.key, { state: 'failed', error: errorMessage(i18n, error) })
      })
      .finally(() => {
        controllers.current.delete(next.key)
        running.current = false
        // Run the effect again for the next one
        setUploads((list) => [...list])
      })
  }, [uploads, usage, onUploaded, i18n, owner])

  const busy = uploads.some((upload) => upload.state === 'waiting' || upload.state === 'uploading')
  // Leaving the page stops them: ask first
  useEffect(() => {
    if (!busy) return
    const warn = (event: BeforeUnloadEvent) => event.preventDefault()
    window.addEventListener('beforeunload', warn)
    return () => window.removeEventListener('beforeunload', warn)
  }, [busy])
  // Gone from the page: nothing goes on in the background
  useEffect(() => {
    const current = controllers.current
    return () => current.forEach((controller) => controller.abort())
  }, [])

  return {
    uploads,
    busy,
    add,
    cancel: (key: number) => {
      controllers.current.get(key)?.abort()
      update(key, { state: 'cancelled' })
    },
    retry: (key: number) => update(key, { state: 'waiting', sent: 0, error: undefined }),
    clear: () => setUploads((list) => list.filter((upload) => upload.state === 'waiting' || upload.state === 'uploading')),
  }
}
