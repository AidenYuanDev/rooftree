import { http, HttpResponse } from 'msw'
import { z } from 'zod'
import { server } from '../../tests/msw'
import { ApiError, api, setCsrfToken } from './api'

const Thing = z.object({ name: z.string() })

describe('api', () => {
  test('checks the response against the schema', async () => {
    server.use(http.get('*/api/thing', () => HttpResponse.json({ name: 'x' })))
    await expect(api('/api/thing', Thing)).resolves.toEqual({ name: 'x' })

    server.use(http.get('*/api/thing', () => HttpResponse.json({ name: 1 })))
    vi.spyOn(console, 'error').mockImplementation(() => {})
    await expect(api('/api/thing', Thing)).rejects.toMatchObject({ code: 'invalid_response' })
  })

  test('turns error bodies into ApiError', async () => {
    server.use(
      http.post('*/api/thing', () =>
        HttpResponse.json({ error: { code: 'validation_failed', fields: { name: 'too_short' } } }, { status: 400 }),
      ),
    )
    const error = await api('/api/thing', Thing, { method: 'POST', body: {} }).catch((e: unknown) => e)
    expect(error).toBeInstanceOf(ApiError)
    expect(error).toMatchObject({ status: 400, code: 'validation_failed', fields: { name: 'too_short' } })
  })

  test('sends the CSRF token with writes only', async () => {
    const seen: (string | null)[] = []
    server.use(
      http.all('*/api/thing', ({ request }) => {
        seen.push(request.headers.get('X-CSRF-Token'))
        return HttpResponse.json({ name: 'x' })
      }),
    )
    setCsrfToken('token')
    await api('/api/thing', Thing)
    await api('/api/thing', Thing, { method: 'PATCH', body: {} })
    expect(seen).toEqual([null, 'token'])
  })

  test('a failed connection is network_error', async () => {
    server.use(http.get('*/api/thing', () => HttpResponse.error()))
    await expect(api('/api/thing', Thing)).rejects.toMatchObject({ code: 'network_error' })
  })
})
