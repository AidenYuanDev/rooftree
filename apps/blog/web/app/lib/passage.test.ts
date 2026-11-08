import { beforeEach, describe, expect, it } from 'vitest'
import { blockText, capture, locate, markerAttribute, maxQuote } from './passage'

let container: HTMLElement

function article(html: string) {
  container.innerHTML = html
}

// A selection from a point to a point, by text node of an element and offset
function select(startNode: Node, startOffset: number, endNode: Node, endOffset: number): Selection {
  const selection = window.getSelection()!
  const range = document.createRange()
  range.setStart(startNode, startOffset)
  range.setEnd(endNode, endOffset)
  selection.removeAllRanges()
  selection.addRange(range)
  return selection
}

const $ = (selector: string) => container.querySelector(selector)!

beforeEach(() => {
  container = document.createElement('div')
  document.body.replaceChildren(container)
})

describe('capture', () => {
  it('takes a selection across emphasis and a link as the page shows it', () => {
    article('<p>开头。</p><p>这里有<strong>加粗的话</strong>和<a href="/x">一个链接</a>。还有一句。</p>')
    const block = container.querySelectorAll('p')[1]!
    // From inside the bold text to inside the link
    const captured = capture(container, select($('strong').firstChild!, 0, $('a').firstChild!, 4), 'zh')
    expect(captured.ok).toBe(true)
    if (!captured.ok) return
    expect(captured.anchor).toEqual({
      lang: 'zh',
      block: 1,
      quote: '加粗的话和一个链接',
      prefix: '这里有',
      suffix: '。还有一句。',
      text: blockText(block),
    })
    expect(captured.range.toString()).toBe('加粗的话和一个链接')
  })

  it('leaves out the whitespace at its ends, keeps the context as it is', () => {
    article('<p>one two three four</p>')
    const text = $('p').firstChild!
    const captured = capture(container, select(text, 3, text, 8), 'en')
    expect(captured.ok && captured.anchor).toMatchObject({ quote: 'two', prefix: 'one ', suffix: ' three four' })
  })

  it('refuses two blocks, outside the article, too long', () => {
    article('<p>first</p><p>second</p>')
    const [first, second] = Array.from(container.querySelectorAll('p'))
    expect(capture(container, select(first!.firstChild!, 0, second!.firstChild!, 3), 'en')).toEqual({ ok: false, error: 'blocks' })

    const outside = document.createElement('p')
    outside.textContent = 'elsewhere'
    document.body.append(outside)
    expect(capture(container, select(outside.firstChild!, 0, outside.firstChild!, 4), 'en')).toEqual({ ok: false, error: 'outside' })

    article(`<p>${'长'.repeat(maxQuote + 1)}</p>`)
    expect(capture(container, select($('p').firstChild!, 0, $('p').firstChild!, maxQuote + 1), 'zh')).toEqual({
      ok: false,
      error: 'tooLong',
    })
  })

  it('takes an item’s paragraph and a code block as blocks', () => {
    article('<ul><li><p>an item</p></li></ul><pre><code>int main() {}</code></pre>')
    const code = $('code').firstChild!
    const captured = capture(container, select(code, 4, code, 8), 'en')
    expect(captured.ok && captured.anchor).toMatchObject({ block: 2, quote: 'main' })
  })
})

describe('locate', () => {
  it('finds a passage in its block, then by its text where the blocks moved', () => {
    article('<p>a</p><p>the quote is here</p>')
    const anchor = { block: 1, quote: 'quote', prefix: 'the ', suffix: ' is here' }
    expect(locate(container, anchor)?.toString()).toBe('quote')

    // A paragraph came before it: its number is a hint only
    article('<p>new</p><p>a</p><p>the quote is here</p>')
    const range = locate(container, anchor)
    expect(range?.startContainer.parentElement).toBe(container.querySelectorAll('p')[2])
  })

  it('tells the same words apart by the text around them', () => {
    article('<p>yes and no, yes and maybe</p>')
    const range = locate(container, { block: 0, quote: 'yes', prefix: 'no, ', suffix: ' and maybe' })
    expect(range?.startOffset).toBe(12)
  })

  it('has none once the post lost its text', () => {
    article('<p>all of it changed</p>')
    expect(locate(container, { block: 0, quote: 'the quote', prefix: '', suffix: '' })).toBeNull()
  })

  it('reads past the markers the page put into the text', () => {
    article(`<p>before <span ${markerAttribute}>💬 3</span>the quote after</p>`)
    expect(blockText($('p'))).toBe('before the quote after')
    expect(locate(container, { block: 0, quote: 'the quote', prefix: 'before ', suffix: ' after' })?.toString()).toBe('the quote')
  })
})
