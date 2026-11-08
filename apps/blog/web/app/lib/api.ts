// The blog API, seen from the browser and from the prerender build.
//
// Every response is checked against a Zod schema (schemas.ts): the server is C++, so the types are not
// shared, and a response that drifts from what the frontend expects fails loudly here instead of
// rendering "undefined" somewhere.
//
// Errors are {"error": {"code", "message"?, "fields"?}}; ApiError carries them, the UI translates the
// codes (i18n errors.* and fieldErrors.*).

import type { z } from 'zod'
import { imageInfo } from './image-info'

// In the browser the API is on the same origin (/api, the server itself or the vite proxy). The build and the dev
// server's loaders run in Node and need an absolute URL
export const apiOrigin = typeof window === 'undefined' ? (process.env.BLOG_API_URL ?? 'http://127.0.0.1:8100') : ''

export class ApiError extends Error {
  readonly status: number
  readonly code: string
  readonly fields: Record<string, string>
  readonly details: Record<string, unknown>
  // Seconds, from Retry-After: when a request that was too soon may be made again
  readonly retryAfter: number | undefined

  constructor(
    status: number,
    code: string,
    fields: Record<string, string> = {},
    details: Record<string, unknown> = {},
    retryAfter?: number,
  ) {
    super(`${status} ${code}`)
    this.status = status
    this.code = code
    this.fields = fields
    this.details = details
    this.retryAfter = retryAfter
  }
}

// The session's CSRF token, from the last /api/auth/me or login response. Sent with every request
// that changes something
let csrfToken: string | null = null

export function setCsrfToken(token: string | null) {
  csrfToken = token
}

// For requests not made through api() (a drive's pieces)
export function getCsrfToken() {
  return csrfToken
}

interface RequestOptions {
  method?: 'GET' | 'POST' | 'PATCH' | 'PUT' | 'DELETE'
  body?: unknown
  signal?: AbortSignal
}

export async function api<T extends z.ZodType>(path: string, schema: T, options: RequestOptions = {}): Promise<z.infer<T>> {
  const method = options.method ?? 'GET'
  const headers: Record<string, string> = {}
  // FormData goes as multipart, fetch sets its Content-Type and boundary
  const form = options.body instanceof FormData
  if (options.body !== undefined && !form) headers['Content-Type'] = 'application/json'
  if (method !== 'GET' && csrfToken) headers['X-CSRF-Token'] = csrfToken

  let response: Response
  const doFetch = () =>
    fetch(`${apiOrigin}${path}`, {
      method,
      headers,
      body: options.body === undefined ? undefined : form ? (options.body as FormData) : JSON.stringify(options.body),
      credentials: 'same-origin',
      signal: options.signal,
    })

  try {
    response = await doFetch()
  } catch (error) {
    if (error instanceof DOMException && error.name === 'AbortError') throw error
    // A socket dropped by a keep-alive timeout or a glitch: once more on a fresh one, but only for a request
    // that may be repeated. A POST or PATCH may have reached the server already (a chat message sent twice)
    if (method === 'POST' || method === 'PATCH') throw new ApiError(0, 'network_error')
    try {
      response = await doFetch()
    } catch {
      throw new ApiError(0, 'network_error')
    }
  }

  const text = await response.text()
  let json: unknown
  try {
    json = text ? JSON.parse(text) : null
  } catch {
    throw new ApiError(response.status, 'invalid_response')
  }

  if (!response.ok) {
    const error = (json as { error?: { code?: unknown; fields?: unknown } } | null)?.error
    const code = typeof error?.code === 'string' ? error.code : `http_${response.status}`
    const fields = error?.fields && typeof error.fields === 'object' ? (error.fields as Record<string, string>) : {}
    const retryAfter = Number(response.headers.get('Retry-After'))
    throw new ApiError(response.status, code, fields, (error ?? {}) as Record<string, unknown>, retryAfter > 0 ? retryAfter : undefined)
  }

  const parsed = schema.safeParse(json)
  if (!parsed.success) {
    console.error(`Unexpected response from ${method} ${path}`, parsed.error.issues)
    throw new ApiError(response.status, 'invalid_response')
  }

  return parsed.data
}

// An admin image upload: multipart, with the CSRF token like every write
// An image for posts: the admin's own, or (postId) one an editor puts in a post, in its owner's drive
export async function uploadFile(file: File, postId?: number) {
  const form = new FormData()
  form.append('file', file)
  const info = await imageInfo(file)
  if (info) {
    form.append('width', String(info.width))
    form.append('height', String(info.height))
    form.append('thumbhash', info.thumbhash)
  }
  let response: Response
  try {
    response = await fetch(`${apiOrigin}${postId === undefined ? '/api/admin/uploads' : `/api/admin/posts/${postId}/uploads`}`, {
      method: 'POST',
      body: form,
      headers: csrfToken ? { 'X-CSRF-Token': csrfToken } : {},
      credentials: 'same-origin',
    })
  } catch {
    throw new ApiError(0, 'network_error')
  }

  const json = (await response.json().catch(() => null)) as {
    url?: string
    error?: { code?: string; fields?: Record<string, string> }
  } | null
  if (!response.ok || typeof json?.url !== 'string')
    throw new ApiError(response.status, json?.error?.code ?? `http_${response.status}`, json?.error?.fields ?? {})
  return { url: json.url }
}
