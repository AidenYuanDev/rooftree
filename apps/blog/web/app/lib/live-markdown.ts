import { syntaxTree } from '@codemirror/language'
import { EditorSelection, type EditorState, type Extension, type Range, StateEffect, StateField, type Text } from '@codemirror/state'
import { Decoration, type DecorationSet, EditorView, keymap, WidgetType } from '@codemirror/view'
import { renderHighlightedLine } from './code-highlight'
import { markdownHtml } from './markdown-html'

// Typora-like live preview on top of a Markdown document in CodeMirror:
// - The cursor's line shows raw Markdown syntax so characters never jump sideways while typing
// - Every other line renders headings, emphasis, links, quotes, lists, tasks, rules and images
// - Multi-line tables and fenced code blocks render as real tables and highlighted code cards when
//   the cursor is outside them, and unfold at the clicked row/line as soon as the cursor enters

const setFocused = StateEffect.define<boolean>()

function isSafeUrl(url: string) {
  return url.startsWith('/uploads/') || url.startsWith('https://') || url.startsWith('http://')
}

function addInlineReplace(out: Range<Decoration>[], doc: Text, from: number, to: number, widget?: WidgetType) {
  if (from >= to || doc.lineAt(from).number !== doc.lineAt(to).number) return
  out.push(Decoration.replace(widget ? { widget } : {}).range(from, to))
}

function focusAt(view: EditorView, pos: number) {
  const clamped = Math.max(0, Math.min(pos, view.state.doc.length))
  view.focus()
  view.dispatch({ selection: { anchor: clamped }, effects: setFocused.of(true) })
}

class BulletWidget extends WidgetType {
  toDOM() {
    const dot = document.createElement('span')
    dot.className = 'cm-lp-bullet'
    dot.textContent = '•'
    dot.setAttribute('aria-hidden', 'true')
    return dot
  }
}

class TaskCheckboxWidget extends WidgetType {
  constructor(
    readonly checked: boolean,
    readonly from: number,
    readonly to: number,
  ) {
    super()
  }

  eq(other: TaskCheckboxWidget) {
    return this.checked === other.checked && this.from === other.from && this.to === other.to
  }

  toDOM(view: EditorView) {
    const box = document.createElement('input')
    box.type = 'checkbox'
    box.checked = this.checked
    box.className = 'cm-lp-checkbox'
    box.addEventListener('mousedown', (event) => {
      event.preventDefault()
      view.dispatch({ changes: { from: this.from, to: this.to, insert: this.checked ? '[ ]' : '[x]' } })
    })
    return box
  }

  ignoreEvent() {
    return true
  }
}

class LinkBadgeWidget extends WidgetType {
  constructor(readonly url: string) {
    super()
  }

  eq(other: LinkBadgeWidget) {
    return this.url === other.url
  }

  toDOM() {
    const icon = document.createElement('a')
    icon.href = this.url
    icon.target = '_blank'
    icon.rel = 'noopener noreferrer'
    icon.className = 'cm-lp-link-open'
    icon.textContent = '↗'
    icon.title = this.url
    icon.addEventListener('mousedown', (event) => event.stopPropagation())
    return icon
  }

  ignoreEvent() {
    return true
  }
}

class ImageWidget extends WidgetType {
  constructor(
    readonly url: string,
    readonly alt: string,
    readonly pos: number,
  ) {
    super()
  }

  eq(other: ImageWidget) {
    return this.url === other.url && this.alt === other.alt
  }

  toDOM(view: EditorView) {
    const wrap = document.createElement('span')
    wrap.className = 'cm-lp-image-wrap'
    const img = document.createElement('img')
    img.src = this.url
    img.alt = this.alt
    img.className = 'cm-lp-image'
    img.loading = 'lazy'
    img.addEventListener('mousedown', (event) => {
      event.preventDefault()
      focusAt(view, this.pos)
    })
    wrap.appendChild(img)
    return wrap
  }
}

class TableWidget extends WidgetType {
  constructor(
    readonly source: string,
    readonly rowOffsets: readonly number[],
    readonly fallbackPos: number,
  ) {
    super()
  }

  eq(other: TableWidget) {
    return this.source === other.source && this.fallbackPos === other.fallbackPos
  }

  toDOM(view: EditorView) {
    const wrap = document.createElement('div')
    wrap.className = 'prose max-w-none prose-night cm-lp-table-wrap'
    // Safe: markdownHtml renders via ReactMarkdown without raw HTML passthrough
    wrap.innerHTML = markdownHtml(this.source)
    for (const anchor of wrap.querySelectorAll('a')) {
      const href = anchor.getAttribute('href') ?? ''
      if (isSafeUrl(href)) {
        anchor.target = '_blank'
        anchor.rel = 'noopener noreferrer'
        anchor.addEventListener('mousedown', (event) => event.stopPropagation())
      } else {
        anchor.removeAttribute('href')
      }
    }
    const rows = wrap.querySelectorAll('tr')
    rows.forEach((row, index) => {
      row.addEventListener('mousedown', (event) => {
        if ((event.target as HTMLElement | null)?.closest('a')) return
        event.preventDefault()
        event.stopPropagation()
        focusAt(view, this.rowOffsets[index] ?? this.fallbackPos)
      })
    })
    wrap.addEventListener('mousedown', (event) => {
      if ((event.target as HTMLElement | null)?.closest('a')) return
      event.preventDefault()
      focusAt(view, this.fallbackPos)
    })
    return wrap
  }

  ignoreEvent() {
    return true
  }
}

class CodeBlockWidget extends WidgetType {
  constructor(
    readonly lang: string,
    readonly lines: readonly string[],
    readonly lineOffsets: readonly number[],
    readonly fallbackPos: number,
  ) {
    super()
  }

  eq(other: CodeBlockWidget) {
    return (
      this.lang === other.lang &&
      this.fallbackPos === other.fallbackPos &&
      this.lines.length === other.lines.length &&
      this.lines.every((line, i) => line === other.lines[i])
    )
  }

  toDOM(view: EditorView) {
    const card = document.createElement('div')
    card.className = 'cm-lp-pre'
    if (this.lang) {
      const badge = document.createElement('span')
      badge.className = 'cm-lp-pre-lang'
      badge.textContent = this.lang
      card.appendChild(badge)
    }
    const pre = document.createElement('pre')
    const code = document.createElement('code')
    if (this.lines.length === 0) {
      const row = document.createElement('div')
      row.className = 'cm-lp-pre-line'
      row.textContent = '​'
      code.appendChild(row)
    } else {
      this.lines.forEach((line, index) => {
        const row = document.createElement('div')
        row.className = 'cm-lp-pre-line'
        row.appendChild(renderHighlightedLine(line, this.lang))
        row.addEventListener('mousedown', (event) => {
          event.preventDefault()
          event.stopPropagation()
          focusAt(view, this.lineOffsets[index] ?? this.fallbackPos)
        })
        code.appendChild(row)
      })
    }
    pre.appendChild(code)
    card.appendChild(pre)
    card.addEventListener('mousedown', (event) => {
      event.preventDefault()
      focusAt(view, this.fallbackPos)
    })
    return card
  }

  ignoreEvent() {
    return true
  }
}

function buildDecorations(state: EditorState, focused: boolean): DecorationSet {
  const { doc } = state
  const activeLines = new Set<number>()
  if (focused) {
    for (const range of state.selection.ranges) {
      const start = doc.lineAt(range.from).number
      const end = doc.lineAt(range.to).number
      for (let n = start; n <= end; n++) activeLines.add(n)
    }
  }
  const touchesLine = (pos: number) => activeLines.has(doc.lineAt(pos).number)
  const touchesBlock = (from: number, to: number) => {
    if (!focused) return false
    const start = doc.lineAt(from).number
    const end = doc.lineAt(to).number
    for (let n = start; n <= end; n++) if (activeLines.has(n)) return true
    return false
  }

  const out: Range<Decoration>[] = []
  const quotedLines = new Set<number>()
  const tree = syntaxTree(state)

  tree.iterate({
    enter(node) {
      const name = node.name

      if (name === 'Table') {
        const firstLine = doc.lineAt(node.from)
        const lastLine = doc.lineAt(node.to)
        const topLevel = doc.sliceString(firstLine.from, node.from).trim() === ''
        if (topLevel && !touchesBlock(firstLine.from, lastLine.to)) {
          const rowOffsets: number[] = []
          const header = node.node.getChild('TableHeader')
          if (header) rowOffsets.push(header.from)
          for (const row of node.node.getChildren('TableRow')) rowOffsets.push(row.from)
          const source = doc.sliceString(firstLine.from, lastLine.to)
          out.push(
            Decoration.replace({ widget: new TableWidget(source, rowOffsets, firstLine.from), block: true }).range(
              firstLine.from,
              lastLine.to,
            ),
          )
          return false
        }
        for (let n = firstLine.number; n <= lastLine.number; n++) {
          out.push(Decoration.line({ class: 'cm-lp-table-raw' }).range(doc.line(n).from))
        }
        return
      }

      if (name === 'TableDelimiter') {
        out.push(Decoration.mark({ class: 'cm-lp-mark' }).range(node.from, node.to))
        return false
      }

      if (name === 'FencedCode') {
        const firstLine = doc.lineAt(node.from)
        const lastLine = doc.lineAt(node.to)
        const topLevel = doc.sliceString(firstLine.from, node.from).trim() === ''
        const info = node.node.getChild('CodeInfo')
        const lang = info ? doc.sliceString(info.from, info.to).trim() : ''

        if (topLevel && lastLine.number > firstLine.number && !touchesBlock(firstLine.from, lastLine.to)) {
          const marks = node.node.getChildren('CodeMark')
          const hasClosingFence = marks.length >= 2 && doc.lineAt(marks[marks.length - 1]!.from).number === lastLine.number
          const codeEndLine = hasClosingFence ? lastLine.number - 1 : lastLine.number
          const lines: string[] = []
          const lineOffsets: number[] = []
          for (let n = firstLine.number + 1; n <= codeEndLine; n++) {
            const line = doc.line(n)
            lines.push(line.text)
            lineOffsets.push(line.from)
          }
          const fallback = lineOffsets[0] ?? firstLine.from
          out.push(
            Decoration.replace({ widget: new CodeBlockWidget(lang, lines, lineOffsets, fallback), block: true }).range(
              firstLine.from,
              lastLine.to,
            ),
          )
          return false
        }

        for (let n = firstLine.number; n <= lastLine.number; n++) {
          const classes = ['cm-lp-codeblock']
          if (n === firstLine.number) classes.push('cm-lp-codeblock-start')
          if (n === lastLine.number) classes.push('cm-lp-codeblock-end')
          out.push(Decoration.line({ class: classes.join(' ') }).range(doc.line(n).from))
        }
        for (const mark of node.node.getChildren('CodeMark')) {
          out.push(Decoration.mark({ class: 'cm-lp-mark' }).range(mark.from, mark.to))
        }
        if (info) out.push(Decoration.mark({ class: 'cm-lp-codeinfo' }).range(info.from, info.to))
        return false
      }

      if (name.startsWith('ATXHeading')) {
        const level = Number(name.slice('ATXHeading'.length))
        if (level >= 1 && level <= 6) {
          const line = doc.lineAt(node.from)
          out.push(Decoration.line({ class: `cm-lp-heading cm-lp-h${level}` }).range(line.from))
          const mark = node.node.getChild('HeaderMark')
          if (mark) {
            const after = Math.min(line.to, doc.sliceString(mark.to, mark.to + 1) === ' ' ? mark.to + 1 : mark.to)
            if (touchesLine(node.from)) out.push(Decoration.mark({ class: 'cm-lp-mark' }).range(mark.from, after))
            else addInlineReplace(out, doc, mark.from, after)
          }
        }
        return
      }

      if (name === 'StrongEmphasis' || name === 'Emphasis' || name === 'Strikethrough') {
        const cls = name === 'StrongEmphasis' ? 'cm-lp-strong' : name === 'Emphasis' ? 'cm-lp-em' : 'cm-lp-strike'
        out.push(Decoration.mark({ class: cls }).range(node.from, node.to))
        const markName = name === 'Strikethrough' ? 'StrikethroughMark' : 'EmphasisMark'
        for (const mark of node.node.getChildren(markName)) {
          if (touchesLine(mark.from)) out.push(Decoration.mark({ class: 'cm-lp-mark' }).range(mark.from, mark.to))
          else addInlineReplace(out, doc, mark.from, mark.to)
        }
        return
      }

      if (name === 'InlineCode') {
        out.push(Decoration.mark({ class: 'cm-lp-code' }).range(node.from, node.to))
        for (const mark of node.node.getChildren('CodeMark')) {
          if (touchesLine(mark.from)) out.push(Decoration.mark({ class: 'cm-lp-mark' }).range(mark.from, mark.to))
          else addInlineReplace(out, doc, mark.from, mark.to)
        }
        return false
      }

      if (name === 'Link') {
        const marks = node.node.getChildren('LinkMark')
        const urlNode = node.node.getChild('URL')
        if (marks.length >= 2 && marks[0]!.to < marks[1]!.from) {
          out.push(Decoration.mark({ class: 'cm-lp-link' }).range(marks[0]!.to, marks[1]!.from))
          const url = urlNode ? doc.sliceString(urlNode.from, urlNode.to).trim() : ''
          if (touchesLine(node.from)) {
            out.push(Decoration.mark({ class: 'cm-lp-mark' }).range(marks[0]!.from, marks[0]!.to))
            out.push(Decoration.mark({ class: 'cm-lp-mark' }).range(marks[1]!.from, node.to))
          } else {
            addInlineReplace(out, doc, marks[0]!.from, marks[0]!.to)
            addInlineReplace(out, doc, marks[1]!.from, node.to, isSafeUrl(url) ? new LinkBadgeWidget(url) : undefined)
          }
        }
        return
      }

      if (name === 'Image') {
        const marks = node.node.getChildren('LinkMark')
        const urlNode = node.node.getChild('URL')
        const alt = marks.length >= 2 ? doc.sliceString(marks[0]!.to, marks[1]!.from) : ''
        const url = urlNode ? doc.sliceString(urlNode.from, urlNode.to).trim() : ''
        if (isSafeUrl(url) && doc.lineAt(node.from).number === doc.lineAt(node.to).number) {
          if (touchesLine(node.from)) {
            out.push(Decoration.mark({ class: 'cm-lp-mark' }).range(node.from, node.to))
            out.push(Decoration.widget({ widget: new ImageWidget(url, alt, node.from), side: 1 }).range(node.to))
          } else {
            addInlineReplace(out, doc, node.from, node.to, new ImageWidget(url, alt, node.from))
          }
        }
        return false
      }

      if (name === 'Blockquote') {
        const startLine = doc.lineAt(node.from).number
        const endLine = doc.lineAt(node.to).number
        for (let n = startLine; n <= endLine; n++) {
          if (quotedLines.has(n)) continue
          quotedLines.add(n)
          out.push(Decoration.line({ class: 'cm-lp-quote' }).range(doc.line(n).from))
        }
        return
      }

      if (name === 'QuoteMark') {
        const line = doc.lineAt(node.from)
        const after = Math.min(line.to, doc.sliceString(node.to, node.to + 1) === ' ' ? node.to + 1 : node.to)
        if (touchesLine(node.from)) out.push(Decoration.mark({ class: 'cm-lp-mark' }).range(node.from, after))
        else addInlineReplace(out, doc, node.from, after)
        return
      }

      if (name === 'ListItem') {
        const listMark = node.node.getChild('ListMark')
        if (!listMark) return
        const active = touchesLine(listMark.from)
        const task = node.node.getChild('Task')
        const taskMarker = task?.getChild('TaskMarker')
        if (taskMarker && doc.lineAt(listMark.from).number === doc.lineAt(taskMarker.to).number) {
          const checked = doc.sliceString(taskMarker.from, taskMarker.to).toLowerCase() === '[x]'
          const line = doc.lineAt(taskMarker.to)
          if (checked && taskMarker.to < line.to) {
            out.push(Decoration.mark({ class: 'cm-lp-task-done' }).range(taskMarker.to, line.to))
          }
          if (active) {
            out.push(Decoration.mark({ class: 'cm-lp-mark' }).range(listMark.from, taskMarker.to))
          } else {
            addInlineReplace(out, doc, listMark.from, taskMarker.to, new TaskCheckboxWidget(checked, taskMarker.from, taskMarker.to))
          }
        } else if (listMark.to - listMark.from === 1) {
          if (active) {
            out.push(Decoration.mark({ class: 'cm-lp-mark' }).range(listMark.from, listMark.to))
          } else {
            addInlineReplace(out, doc, listMark.from, listMark.to, new BulletWidget())
          }
        } else {
          out.push(Decoration.mark({ class: 'cm-lp-mark' }).range(listMark.from, listMark.to))
        }
        return
      }

      if (name === 'HorizontalRule') {
        const line = doc.lineAt(node.from)
        if (touchesLine(node.from)) {
          out.push(Decoration.mark({ class: 'cm-lp-mark' }).range(node.from, node.to))
        } else {
          out.push(Decoration.line({ class: 'cm-lp-hr' }).range(line.from))
          addInlineReplace(out, doc, line.from, line.to)
        }
        return false
      }
    },
  })

  return Decoration.set(out, true)
}

const liveMarkdownField = StateField.define<{ focused: boolean; decorations: DecorationSet }>({
  create(state) {
    return { focused: false, decorations: buildDecorations(state, false) }
  },
  update(value, tr) {
    let focused = value.focused
    let focusChanged = false
    for (const effect of tr.effects) {
      if (effect.is(setFocused) && effect.value !== focused) {
        focused = effect.value
        focusChanged = true
      }
    }
    if (!tr.docChanged && !tr.selection && !focusChanged && syntaxTree(tr.startState) === syntaxTree(tr.state)) {
      return value
    }
    return { focused, decorations: buildDecorations(tr.state, focused) }
  },
  provide: (field) => EditorView.decorations.from(field, (value) => value.decorations),
})

const focusHandlers = EditorView.domEventHandlers({
  focus(_event, view) {
    view.dispatch({ effects: setFocused.of(true) })
  },
  blur(_event, view) {
    view.dispatch({ effects: setFocused.of(false) })
  },
})

// Toggles symmetric inline marks (**bold**, *italic*, `code`) around the selection
export function toggleInlineMark(
  state: EditorState,
  dispatch: ((tr: ReturnType<EditorState['update']>) => void) | undefined,
  mark: string,
) {
  const len = mark.length
  const changes = state.changeByRange((range) => {
    const before = state.doc.sliceString(Math.max(0, range.from - len), range.from)
    const after = state.doc.sliceString(range.to, Math.min(state.doc.length, range.to + len))
    if (before === mark && after === mark) {
      return {
        changes: [
          { from: range.from - len, to: range.from, insert: '' },
          { from: range.to, to: range.to + len, insert: '' },
        ],
        range: EditorSelection.range(range.from - len, range.to - len),
      }
    }
    const text = state.doc.sliceString(range.from, range.to)
    if (text.startsWith(mark) && text.endsWith(mark) && text.length >= len * 2) {
      return {
        changes: { from: range.from, to: range.to, insert: text.slice(len, -len) },
        range: EditorSelection.range(range.from, range.to - len * 2),
      }
    }
    return {
      changes: { from: range.from, to: range.to, insert: `${mark}${text}${mark}` },
      range: EditorSelection.range(range.from + len, range.to + len),
    }
  })
  dispatch?.(state.update(changes, { scrollIntoView: true, userEvent: 'input' }))
  return true
}

function insertLink(view: EditorView) {
  const { state } = view
  const range = state.selection.main
  const selected = state.doc.sliceString(range.from, range.to)
  const insert = `[${selected}](https://)`
  const urlStart = range.from + selected.length + 3
  const urlEnd = urlStart + 'https://'.length
  view.dispatch({
    changes: { from: range.from, to: range.to, insert },
    selection: { anchor: urlStart, head: urlEnd },
    scrollIntoView: true,
    userEvent: 'input',
  })
  return true
}

export const markdownShortcuts: Extension = keymap.of([
  { key: 'Mod-b', run: (view) => toggleInlineMark(view.state, view.dispatch, '**') },
  { key: 'Mod-i', run: (view) => toggleInlineMark(view.state, view.dispatch, '*') },
  { key: 'Mod-e', run: (view) => toggleInlineMark(view.state, view.dispatch, '`') },
  { key: 'Mod-k', run: insertLink },
])

export function liveMarkdown(): Extension {
  return [liveMarkdownField, focusHandlers]
}

export const editorTheme = EditorView.theme({
  '&': { backgroundColor: 'transparent', color: 'var(--foreground)' },
  '&.cm-focused': { outline: 'none' },
  '.cm-scroller': { fontFamily: 'inherit', lineHeight: 'inherit' },
  '.cm-content': { caretColor: 'var(--foreground)', fontFamily: 'inherit', padding: '0.6rem 0' },
  '.cm-line': { padding: '0 0.85rem' },
  '.cm-cursor': { borderLeftColor: 'var(--foreground)' },
  '.cm-placeholder': { color: 'var(--muted-foreground)' },
  '&.cm-focused .cm-selectionBackground, .cm-selectionBackground, ::selection': {
    backgroundColor: 'color-mix(in oklch, var(--primary) 22%, transparent)',
  },
})
