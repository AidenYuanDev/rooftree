import { rgbaToThumbHash } from 'thumbhash'

// An image's size and blurred placeholder (thumbhash, base64), computed before it's uploaded: the
// server keeps them and the build puts them into posts. null when the browser can't decode it
export async function imageInfo(file: Blob): Promise<{ width: number; height: number; thumbhash: string } | null> {
  try {
    const bitmap = await createImageBitmap(file)
    const { width, height } = bitmap
    // thumbhash takes at most 100x100
    const scale = Math.min(100 / width, 100 / height, 1)
    const w = Math.max(1, Math.round(width * scale))
    const h = Math.max(1, Math.round(height * scale))
    const canvas = document.createElement('canvas')
    canvas.width = w
    canvas.height = h
    const context = canvas.getContext('2d')
    if (!context) return null
    context.drawImage(bitmap, 0, 0, w, h)
    bitmap.close()

    const hash = rgbaToThumbHash(w, h, context.getImageData(0, 0, w, h).data)
    return { width: Math.min(width, 65535), height: Math.min(height, 65535), thumbhash: btoa(String.fromCharCode(...hash)) }
  } catch {
    return null
  }
}
