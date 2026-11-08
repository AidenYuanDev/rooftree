import { http, HttpResponse } from 'msw'
import { server } from '../../tests/msw'
import { fileKind, folderTree, formatBytes, isValidName, uploadToDrive } from './drive'

describe('drive', () => {
  test('sizes', () => {
    expect(formatBytes(0, 'en')).toBe('0 B')
    expect(formatBytes(1536, 'en')).toBe('1.5 KB')
    expect(formatBytes(200 * 1024 * 1024, 'zh')).toBe('200 MB')
    expect(formatBytes(2 * 1024 ** 3, 'en')).toBe('2 GB')
  })

  test('what the page shows itself', () => {
    expect(fileKind('image/png')).toBe('image')
    expect(fileKind('image/svg+xml')).toBe('other')
    expect(fileKind('video/mp4')).toBe('video')
    expect(fileKind('application/pdf')).toBe('pdf')
    expect(fileKind('text/html')).toBe('other')
  })

  test('names the server takes', () => {
    expect(isValidName('照片 (2).jpg')).toBe(true)
    for (const name of ['', '  ', '.', '..', 'a/b', 'a\\b', 'tab\there', 'x'.repeat(256)]) expect(isValidName(name)).toBe(false)
  })

  test('folders as a tree, without the one being moved and what is under it', () => {
    const folder = (id: number, parentId: number | null, name: string) => ({ id, parentId, name, createdAt: '' })
    const folders = [folder(1, null, 'b'), folder(2, null, 'a'), folder(3, 1, 'c'), folder(4, 3, 'd')]
    expect(folderTree(folders).map(({ folder: f, depth }) => `${depth}${f.name}`)).toEqual(['0a', '0b', '1c', '2d'])
    expect(folderTree(folders, 3).map(({ folder: f }) => f.name)).toEqual(['a', 'b'])
  })

  test('uploads in pieces, going on from where the server got to', async () => {
    const data = new Uint8Array(10).map((_, i) => i)
    const received: number[] = []
    const offsets: number[] = []
    let lostAnswer = false
    server.use(
      http.post('*/api/drive/uploads', async ({ request }) => {
        expect(await request.json()).toEqual({ folderId: 7, name: 'a.bin', size: 10 })
        return HttpResponse.json({ id: 'u1', received: 0, chunkSize: 4 }, { status: 201 })
      }),
      http.put('*/api/drive/uploads/u1', async ({ request }) => {
        const offset = Number(new URL(request.url).searchParams.get('offset'))
        offsets.push(offset)
        if (offset !== received.length)
          return HttpResponse.json({ error: { code: 'offset_mismatch', fields: { received: String(received.length) } } }, { status: 409 })
        received.push(...new Uint8Array(await request.arrayBuffer()))
        // The second piece arrives, its answer doesn't: sent again, the server says it has it
        if (offset === 4 && !lostAnswer) {
          lostAnswer = true
          return HttpResponse.error()
        }
        return HttpResponse.json({ received: received.length })
      }),
      http.post('*/api/drive/uploads/u1/complete', () =>
        HttpResponse.json(
          { id: 1, folderId: 7, name: 'a.bin', size: 10, mime: 'application/octet-stream', createdAt: '', updatedAt: '', thumb: false },
          { status: 201 },
        ),
      ),
    )
    vi.useFakeTimers({ toFake: ['setTimeout'] })
    const progress: number[] = []
    const upload = uploadToDrive(new File([data], 'a.bin'), {
      folderId: 7,
      chunkSize: 8 * 1024 * 1024,
      signal: new AbortController().signal,
      onProgress: (sent) => progress.push(sent),
    })
    await vi.waitFor(async () => {
      await vi.advanceTimersByTimeAsync(5000)
      expect(offsets.length).toBeGreaterThanOrEqual(4)
    })
    const file = await upload
    vi.useRealTimers()
    expect(file.name).toBe('a.bin')
    expect(received).toEqual([...data])
    expect(offsets).toEqual([0, 4, 4, 8])
    expect(progress.at(-1)).toBe(10)
  })

  test('a cancelled upload is forgotten by the server too', async () => {
    let deleted = false
    const controller = new AbortController()
    server.use(
      http.post('*/api/drive/uploads', () => HttpResponse.json({ id: 'u2', received: 0, chunkSize: 4 }, { status: 201 })),
      http.put('*/api/drive/uploads/u2', () => {
        controller.abort()
        return new Promise(() => {})
      }),
      http.delete('*/api/drive/uploads/u2', () => {
        deleted = true
        return HttpResponse.json({ ok: true })
      }),
    )
    const upload = uploadToDrive(new File([new Uint8Array(10)], 'b.bin'), {
      folderId: null,
      chunkSize: 4,
      signal: controller.signal,
      onProgress: () => {},
    })
    await expect(upload).rejects.toMatchObject({ name: 'AbortError' })
    await vi.waitFor(() => expect(deleted).toBe(true))
  })
})
