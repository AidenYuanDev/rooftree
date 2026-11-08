import type { CommentAnchorInput, Lang } from '~/lib/schemas'

// A passage of a post: the selected text of one block, found again by its text and the text around it
// (the block's number a hint only), so a post that changed elsewhere still has it. The server checks
// the same limits (CommentHandlers.cpp) and looks for the quote in the post's Markdown when it changes.

// Blocks a passage is in. A selection's block is the nearest one around it: an item's paragraph, not
// the item
export const blockSelector = 'p, li, h1, h2, h3, h4, h5, h6, pre, td, th, dt, dd, figcaption'
// What the page put into the text itself (the 💬 markers): never part of a block's text
export const markerAttribute = 'data-passage-marker'

export const maxQuote = 300
export const contextLength = 32
export const maxBlockText = 4000

// Characters, not UTF-16 units: the server counts the same
const chars = (text: string) => Array.from(text)

// The text nodes of a block that are its text, in order
function textNodes(block: Element): Text[] {
  const nodes: Text[] = []
  const walker = block.ownerDocument.createTreeWalker(block, NodeFilter.SHOW_TEXT, {
    acceptNode: (node) => (node.parentElement?.closest(`[${markerAttribute}]`) ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT),
  })
  for (let node = walker.nextNode(); node; node = walker.nextNode()) nodes.push(node as Text)
  return nodes
}

export function blockText(block: Element): string {
  return textNodes(block)
    .map((node) => node.data)
    .join('')
}

export function blocksOf(container: Element): Element[] {
  return Array.from(container.querySelectorAll(blockSelector))
}

function blockOf(node: Node, container: Element): Element | null {
  const element = node.nodeType === Node.ELEMENT_NODE ? (node as Element) : node.parentElement
  const block = element?.closest(blockSelector)
  return block && container.contains(block) ? block : null
}

// Where a point of the DOM is in its block's text
function offsetIn(block: Element, node: Node, offset: number): number {
  const point = block.ownerDocument.createRange()
  point.setStart(node, offset)
  let at = 0
  for (const text of textNodes(block)) {
    if (text === node) return at + offset
    // A text node from the point on: the ones before it are all counted
    if (point.comparePoint(text, 0) >= 0) return at
    at += text.data.length
  }
  return at
}

// The range of [start, end) of a block's text
export function rangeOf(block: Element, start: number, end: number): Range {
  const range = block.ownerDocument.createRange()
  let at = 0
  let started = false
  const nodes = textNodes(block)
  const last = nodes.at(-1)
  for (const node of nodes) {
    const next = at + node.data.length
    // A start at a node's end is the next node's start: what is between them (a marker) isn't in it
    if (!started && (start < next || (start === next && node === last))) {
      range.setStart(node, start - at)
      started = true
    }
    if (started && end <= next) {
      range.setEnd(node, end - at)
      return range
    }
    at = next
  }
  if (last) range.setEnd(last, last.data.length)
  return range
}

export type CaptureError = 'empty' | 'outside' | 'blocks' | 'tooLong'

export type Captured = { ok: true; anchor: CommentAnchorInput; range: Range } | { ok: false; error: CaptureError }

// The selection as a passage, if it is one: inside the container, in one block, at most maxQuote
// characters once its ends' whitespace is left out
export function capture(container: Element, selection: Selection | null, lang: Lang): Captured {
  if (!selection || selection.rangeCount === 0 || selection.isCollapsed) return { ok: false, error: 'empty' }
  const range = selection.getRangeAt(0)
  const block = blockOf(range.startContainer, container)
  if (!block) return { ok: false, error: 'outside' }
  if (blockOf(range.endContainer, container) !== block) return { ok: false, error: 'blocks' }

  const text = blockText(block)
  let start = offsetIn(block, range.startContainer, range.startOffset)
  let end = offsetIn(block, range.endContainer, range.endOffset)
  while (start < end && /\s/.test(text[start] ?? '')) start++
  while (end > start && /\s/.test(text[end - 1] ?? '')) end--
  if (start >= end) return { ok: false, error: 'empty' }

  const quote = text.slice(start, end)
  if (chars(quote).length > maxQuote) return { ok: false, error: 'tooLong' }

  // A block longer than the server keeps: the part around the quote
  let snapshot = text
  if (chars(text).length > maxBlockText) {
    const before = chars(text.slice(0, start))
    const keepBefore = Math.min(before.length, Math.floor((maxBlockText - chars(quote).length) / 2))
    snapshot = chars(text)
      .slice(before.length - keepBefore, before.length - keepBefore + maxBlockText)
      .join('')
  }

  const anchor: CommentAnchorInput = {
    lang,
    block: blocksOf(container).indexOf(block),
    quote,
    prefix: chars(text.slice(0, start)).slice(-contextLength).join(''),
    suffix: chars(text.slice(end)).slice(0, contextLength).join(''),
    text: snapshot,
  }
  return { ok: true, anchor, range: rangeOf(block, start, end) }
}

// Where a passage is now: the occurrence of its quote whose text around it matches best, in its own
// block first. None when the post no longer has its quote
export function locate(container: Element, anchor: Pick<CommentAnchorInput, 'block' | 'quote' | 'prefix' | 'suffix'>): Range | null {
  const blocks = blocksOf(container)
  const hinted = blocks[anchor.block]
  const ordered = hinted ? [hinted, ...blocks.filter((block) => block !== hinted)] : blocks
  const prefix = anchor.prefix.trim()
  const suffix = anchor.suffix.trim()

  let best: { block: Element; start: number; score: number } | null = null
  for (const block of ordered) {
    const text = blockText(block)
    for (let at = text.indexOf(anchor.quote); at !== -1; at = text.indexOf(anchor.quote, at + 1)) {
      const before = text.slice(0, at).trim()
      const after = text.slice(at + anchor.quote.length).trim()
      const score =
        (prefix === '' || before.endsWith(prefix) ? 2 : 0) +
        (suffix === '' || after.startsWith(suffix) ? 2 : 0) +
        (block === hinted ? 1 : 0)
      if (!best || score > best.score) best = { block, start: at, score }
    }
    // Its own block with its text around it: nothing elsewhere is better
    if (best?.score === 5) break
  }
  return best ? rangeOf(best.block, best.start, best.start + anchor.quote.length) : null
}

// One highlighted passage at a time, by the CSS Custom Highlight API (::highlight(passage) in app.css):
// the text isn't touched. Browsers without it show none
export function highlight(range: Range | null) {
  if (typeof CSS === 'undefined' || !('highlights' in CSS)) return
  if (range) CSS.highlights.set('passage', new Highlight(range))
  else CSS.highlights.delete('passage')
}
