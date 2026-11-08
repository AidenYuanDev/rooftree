// Accounts' pictures. An upload is cropped and encoded here: the middle square of the image at 256
// pixels, as WebP (PNG where the browser can't write WebP). Drawing it again also drops whatever the
// file carried along, like where a photo was taken.

export const avatarSize = 256

export async function squareAvatar(file: Blob): Promise<Blob> {
  const bitmap = await createImageBitmap(file)
  const side = Math.min(bitmap.width, bitmap.height)
  const canvas = document.createElement('canvas')
  canvas.width = avatarSize
  canvas.height = avatarSize
  const context = canvas.getContext('2d')
  if (!context) throw new Error('no 2d canvas')
  context.imageSmoothingQuality = 'high'
  context.drawImage(bitmap, (bitmap.width - side) / 2, (bitmap.height - side) / 2, side, side, 0, 0, avatarSize, avatarSize)
  bitmap.close()
  const blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, 'image/webp', 0.85))
  if (!blob) throw new Error('the canvas made no picture')
  return blob
}

// The initials' background: one of a few hues by the account's id, the same everywhere
const hues = [12, 38, 95, 150, 190, 220, 265, 320]

export function avatarHue(id: number) {
  return hues[id % hues.length]
}

// The first character of the name (a whole one, also outside the BMP), upper case
export function initials(name: string) {
  const first = [...name.trim()][0] ?? '?'
  return first.toUpperCase()
}
