// Small pictures of a drive's pictures and videos, made in the browser (the server never decodes an
// image): at most Edge pixels on the long side, WebP where the browser writes it (Safari doesn't: JPEG),
// under the server's 128 KiB. Sent after the upload, or when a file without one is opened
import { apiOrigin, getCsrfToken } from './api'
import { drivePath, fileKind } from './drive'

const Edge = 320
const MaxBytes = 128 * 1024
// Pictures larger than this aren't decoded for one (the memory a big decode takes)
const MaxPictureBytes = 40 * 1024 * 1024

function toBlob(canvas: HTMLCanvasElement, type: string, quality: number) {
  return new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, type, quality))
}

// The frame a second in (or a tenth of a short one)
function videoFrame(file: Blob) {
  return new Promise<HTMLVideoElement>((resolve, reject) => {
    const video = document.createElement('video')
    video.muted = true
    video.playsInline = true
    video.preload = 'auto'
    const url = URL.createObjectURL(file)
    const timer = window.setTimeout(() => done(new Error('timeout')), 15000)
    const done = (error?: Error) => {
      window.clearTimeout(timer)
      URL.revokeObjectURL(url)
      if (error) reject(error)
      else resolve(video)
    }
    video.onloadedmetadata = () => {
      video.currentTime = Math.min(1, (video.duration || 0) / 10)
    }
    video.onseeked = () => done()
    video.onerror = () => done(new Error('video'))
    video.src = url
  })
}

// null when the browser can't read it
export async function makeThumbnail(file: Blob, mime: string): Promise<Blob | null> {
  const kind = fileKind(mime)
  if (kind !== 'image' && kind !== 'video') return null
  if (kind === 'image' && file.size > MaxPictureBytes) return null
  try {
    const source = kind === 'image' ? await createImageBitmap(file) : await videoFrame(file)
    const width = source instanceof HTMLVideoElement ? source.videoWidth : source.width
    const height = source instanceof HTMLVideoElement ? source.videoHeight : source.height
    if (!width || !height) return null
    const scale = Math.min(1, Edge / Math.max(width, height))
    const canvas = document.createElement('canvas')
    canvas.width = Math.max(1, Math.round(width * scale))
    canvas.height = Math.max(1, Math.round(height * scale))
    const context = canvas.getContext('2d')
    if (!context) return null
    // JPEG has no transparency: white under it
    context.fillStyle = '#fff'
    context.fillRect(0, 0, canvas.width, canvas.height)
    context.drawImage(source, 0, 0, canvas.width, canvas.height)
    if (source instanceof ImageBitmap) source.close()

    for (const quality of [0.8, 0.6, 0.4]) {
      let blob = await toBlob(canvas, 'image/webp', quality)
      if (blob?.type !== 'image/webp') blob = await toBlob(canvas, 'image/jpeg', quality)
      if (blob && blob.size <= MaxBytes) return blob
    }
    return null
  } catch {
    return null
  }
}

export function thumbUrl(fileId: number, { token, owner }: { token?: string; owner?: number } = {}) {
  return `${apiOrigin}${token ? `/api/shares/${token}/files/${fileId}/thumb` : drivePath(`/api/drive/files/${fileId}/thumb`, owner)}`
}

// Made and sent; false when it couldn't be (no harm: the list shows the file's icon)
export async function sendThumbnail(fileId: number, file: Blob, mime: string, owner?: number) {
  const thumb = await makeThumbnail(file, mime)
  if (!thumb) return false
  const token = getCsrfToken()
  try {
    const response = await fetch(`${apiOrigin}${drivePath(`/api/drive/files/${fileId}/thumb`, owner)}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/octet-stream', ...(token ? { 'X-CSRF-Token': token } : {}) },
      body: thumb,
      credentials: 'same-origin',
    })
    return response.ok
  } catch {
    return false
  }
}
