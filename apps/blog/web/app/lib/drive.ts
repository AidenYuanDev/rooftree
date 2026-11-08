// Drives: sizes, what the browser can show, the addresses of files, and uploads in pieces.
//
// An upload is made on the server (POST /api/drive/uploads), then sent in pieces of chunkSize, each its
// own request with the offset the server has so far: a dropped connection loses at most one piece, and
// the server answers 409 with where it got to when a piece was sent twice. The last request makes the
// file (".../complete"), named "name (2)" when the name was taken meanwhile.

import { ApiError, apiOrigin, getCsrfToken } from './api'
import { toast } from 'sonner'
import { DriveFile, type DriveFolder } from './schemas'

export function formatBytes(bytes: number, lang: string): string {
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  let value = bytes
  let unit = 0
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024
    ++unit
  }
  const digits = unit === 0 || value >= 100 ? 0 : 1
  return `${value.toLocaleString(lang === 'zh' ? 'zh-CN' : 'en-US', { maximumFractionDigits: digits })} ${units[unit]}`
}

// A date and time in the reader's zone; the drive's pages are never prerendered
export function formatDate(iso: string, lang: string) {
  return new Intl.DateTimeFormat(lang === 'zh' ? 'zh-CN' : 'en-US', { dateStyle: 'medium', timeStyle: 'short' }).format(new Date(iso))
}

export type FileKind = 'image' | 'video' | 'audio' | 'pdf' | 'other'

// What the page can show itself; the server sends the rest only as downloads (SVG too: it may run scripts)
export function fileKind(mime: string): FileKind {
  if (mime === 'image/svg+xml') return 'other'
  if (mime.startsWith('image/')) return 'image'
  if (mime.startsWith('video/')) return 'video'
  if (mime.startsWith('audio/')) return 'audio'
  if (mime === 'application/pdf') return 'pdf'
  return 'other'
}

// An address of the drive API, of another account's drive for the admin (owner: its id)
export function drivePath(path: string, owner?: number) {
  return owner === undefined ? path : `${path}${path.includes('?') ? '&' : '?'}user=${owner}`
}

// A file of the drive (another account's for the admin), or of a link (token); download: as a
// download, else shown
export function fileUrl(fileId: number, { token, download = false, owner }: { token?: string; download?: boolean; owner?: number } = {}) {
  const base = token ? `/api/shares/${token}/files/${fileId}` : drivePath(`/api/drive/files/${fileId}`, owner)
  return `${apiOrigin}${base}${download ? `${base.includes('?') ? '&' : '?'}download=1` : ''}`
}

// A zip, made by the server as it goes out: a folder with all under it, the files and folders chosen
// (named name.zip), or the whole drive
export function zipUrl({
  folder,
  files = [],
  folders = [],
  name,
  owner,
}: { folder?: number; files?: number[]; folders?: number[]; name?: string; owner?: number } = {}) {
  const query = new URLSearchParams()
  if (folder) query.set('folder', String(folder))
  if (files.length) query.set('files', files.join(','))
  if (folders.length) query.set('folders', folders.join(','))
  if (name) query.set('name', name)
  const search = query.toString()
  return `${apiOrigin}${drivePath(`/api/drive/zip${search ? `?${search}` : ''}`, owner)}`
}

// A folder link's zip: its folder, or one under it
export function shareZipUrl(token: string, folder: number | null) {
  return `${apiOrigin}/api/shares/${token}/zip${folder ? `?folder=${folder}` : ''}`
}

export function shareUrl(lang: string, token: string) {
  return `${typeof window === 'undefined' ? '' : window.location.origin}/${lang}/s/${token}`
}

// A name for a file or folder, as the server takes it: no / or \, not . or .., no control characters
export function isValidName(name: string) {
  const trimmed = name.trim()
  // eslint-disable-next-line no-control-regex
  return trimmed !== '' && trimmed !== '.' && trimmed !== '..' && [...trimmed].length <= 255 && !/[/\\\u0000-\u001f\u007f]/.test(trimmed)
}

// Every folder as a tree, depth first; without `skip` and what is under it (a folder can't go into itself)
export function folderTree(folders: DriveFolder[], skip?: number) {
  const children = new Map<number | null, DriveFolder[]>()
  for (const folder of folders) children.set(folder.parentId, [...(children.get(folder.parentId) ?? []), folder])
  const out: { folder: DriveFolder; depth: number }[] = []
  const walk = (parent: number | null, depth: number) => {
    for (const folder of (children.get(parent) ?? []).sort((a, b) => a.name.localeCompare(b.name))) {
      if (folder.id === skip) continue
      out.push({ folder, depth })
      walk(folder.id, depth + 1)
    }
  }
  walk(null, 0)
  return out
}

export async function copyLink(url: string, done: string) {
  try {
    await navigator.clipboard.writeText(url)
    toast.success(done)
  } catch {
    // No clipboard (not https, or not allowed): the link is in the field to copy by hand
  }
}

interface JsonError {
  error?: { code?: string; fields?: Record<string, string> }
}

function errorOf(status: number, text: string): ApiError {
  let json: JsonError | null = null
  try {
    json = JSON.parse(text) as JsonError
  } catch {
    // not JSON: a proxy's page
  }
  return new ApiError(status, json?.error?.code ?? `http_${status}`, json?.error?.fields ?? {})
}

async function send(method: string, path: string, body?: unknown, signal?: AbortSignal): Promise<unknown> {
  const token = getCsrfToken()
  let response: Response
  try {
    response = await fetch(`${apiOrigin}${path}`, {
      method,
      headers: { 'Content-Type': 'application/json', ...(token ? { 'X-CSRF-Token': token } : {}) },
      body: body === undefined ? undefined : JSON.stringify(body),
      credentials: 'same-origin',
      signal,
    })
  } catch (error) {
    if (error instanceof DOMException && error.name === 'AbortError') throw error
    throw new ApiError(0, 'network_error')
  }
  const text = await response.text()
  if (!response.ok) throw errorOf(response.status, text)
  return text ? JSON.parse(text) : null
}

// One piece, with its progress (fetch can't tell how much of a body went out)
function sendPiece(path: string, piece: Blob, onProgress: (sent: number) => void, signal: AbortSignal) {
  return new Promise<number>((resolve, reject) => {
    const request = new XMLHttpRequest()
    request.open('PUT', `${apiOrigin}${path}`)
    request.withCredentials = true
    request.setRequestHeader('Content-Type', 'application/octet-stream')
    const token = getCsrfToken()
    if (token) request.setRequestHeader('X-CSRF-Token', token)
    request.upload.onprogress = (event) => onProgress(event.loaded)
    request.onload = () => {
      if (request.status === 200) return resolve((JSON.parse(request.responseText) as { received: number }).received)
      reject(errorOf(request.status, request.responseText))
    }
    request.onerror = () => reject(new ApiError(0, 'network_error'))
    const abort = () => request.abort()
    request.onabort = () => reject(new DOMException('Aborted', 'AbortError'))
    signal.addEventListener('abort', abort, { once: true })
    request.send(piece)
  })
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms))

// Errors worth sending the piece again for: the connection, the server being busy or restarting
function isTransient(error: unknown) {
  return error instanceof ApiError && (error.status === 0 || error.status === 429 || error.status >= 502)
}

export interface UploadOptions {
  folderId: number | null
  // Into another account's drive, for the admin
  owner?: number
  chunkSize: number
  // Bytes the server has, after every bit of progress
  onProgress: (sent: number) => void
  signal: AbortSignal
}

// The whole upload of a file; cancelled (signal), the server forgets it too
export async function uploadToDrive(file: File, { folderId, owner, chunkSize, onProgress, signal }: UploadOptions): Promise<DriveFile> {
  const created = (await send('POST', drivePath('/api/drive/uploads', owner), { folderId, name: file.name, size: file.size }, signal)) as {
    id: string
    chunkSize: number
  }
  const size = Math.min(chunkSize, created.chunkSize)
  try {
    let offset = 0
    let failures = 0
    while (offset < file.size) {
      try {
        const start = offset
        offset = await sendPiece(
          drivePath(`/api/drive/uploads/${created.id}?offset=${offset}`, owner),
          file.slice(offset, offset + size),
          (sent) => onProgress(start + sent),
          signal,
        )
        failures = 0
        onProgress(offset)
      } catch (error) {
        // Sent before and its answer lost: go on from where the server is
        if (error instanceof ApiError && error.code === 'offset_mismatch' && error.fields.received) {
          offset = Number(error.fields.received)
          continue
        }
        if (!isTransient(error) || ++failures > 5) throw error
        await sleep(1000 * 2 ** failures)
      }
    }
    return DriveFile.parse(await send('POST', drivePath(`/api/drive/uploads/${created.id}/complete`, owner), {}, signal))
  } catch (error) {
    // Not left counting against the quota for a day
    void send('DELETE', drivePath(`/api/drive/uploads/${created.id}`, owner)).catch(() => undefined)
    throw error
  }
}
