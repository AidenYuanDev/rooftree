import { markdown, markdownLanguage } from '@codemirror/lang-markdown'
import { Compartment, EditorState } from '@codemirror/state'
import { EditorView } from '@codemirror/view'
import { liveMarkdown, toggleInlineMark } from './live-markdown'

function makeEditor(doc: string, cursor = doc.length) {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const compartment = new Compartment()
  const view = new EditorView({
    state: EditorState.create({
      doc,
      selection: { anchor: cursor },
      extensions: [markdown({ base: markdownLanguage }), compartment.of(liveMarkdown())],
    }),
    parent: host,
  })
  return {
    view,
    host,
    setSourceMode: (on: boolean) => view.dispatch({ effects: compartment.reconfigure(on ? [] : liveMarkdown()) }),
    cleanup: () => {
      view.destroy()
      host.remove()
    },
  }
}

test('folds Markdown marks away when unfocused and styles headings, bold, code, quotes, tasks and links', () => {
  const sample = [
    '# Heading One',
    '',
    'Some **bold**, *italic*, ~~gone~~, `code` and [site](https://example.com).',
    '',
    '> quoted words',
    '',
    '- bullet',
    '- [ ] pending',
    '- [x] finished',
    '',
    '---',
    '',
    '| Need | Way |',
    '| --- | --- |',
    '| GNU tools | MSYS2 |',
    '',
    '```bash',
    '# mirror comment',
    'sed -i "s#old#new#g" file',
    '```',
  ].join('\n')

  const { view, host, cleanup } = makeEditor(sample)
  try {
    expect(host.querySelector('.cm-lp-h1')?.textContent).toBe('Heading One')
    expect(host.querySelector('.cm-lp-strong')?.textContent).toBe('bold')
    expect(host.querySelector('.cm-lp-em')?.textContent).toBe('italic')
    expect(host.querySelector('.cm-lp-strike')?.textContent).toBe('gone')
    expect(host.querySelector('.cm-lp-code')?.textContent).toBe('code')
    expect(host.querySelector('.cm-lp-link')?.textContent).toBe('site')
    expect(host.querySelector('.cm-lp-quote')?.textContent).toBe('quoted words')
    expect(host.querySelectorAll('.cm-lp-checkbox')).toHaveLength(2)
    expect(host.querySelector('.cm-lp-hr')).not.toBeNull()

    // Table renders as a real HTML <table> when cursor is outside
    const table = host.querySelector('.cm-lp-table-wrap table')
    expect(table).not.toBeNull()
    expect(table?.querySelectorAll('th')).toHaveLength(2)
    expect(table?.querySelector('td')?.textContent).toBe('GNU tools')

    // Code block renders as a highlighted <pre> card (with string & comment tokens, not # inside string)
    const pre = host.querySelector('.cm-lp-pre')
    expect(pre).not.toBeNull()
    expect(pre?.querySelector('.cm-lp-pre-lang')?.textContent).toBe('bash')
    expect(pre?.querySelector('.cm-tok-comment')?.textContent).toBe('# mirror comment')
    expect(pre?.querySelector('.cm-tok-string')?.textContent).toBe('"s#old#new#g"')
    expect(pre?.querySelector('.cm-tok-flag')?.textContent).toBe('-i')
    expect(pre?.querySelector('.cm-tok-keyword')?.textContent).toBe('sed')

    // Clicking a table row unfolds the Markdown table at that row
    const firstRow = table!.querySelector('tbody tr')!
    firstRow.dispatchEvent(new MouseEvent('mousedown', { bubbles: true, cancelable: true }))
    expect(host.querySelector('.cm-lp-table-wrap')).toBeNull()
    expect(host.textContent).toContain('| GNU tools | MSYS2 |')

    // Clicking the task checkbox updates the underlying Markdown text
    const firstBox = host.querySelector('.cm-lp-checkbox') as HTMLInputElement
    expect(firstBox.checked).toBe(false)
    firstBox.dispatchEvent(new MouseEvent('mousedown', { bubbles: true, cancelable: true }))
    expect(view.state.doc.toString()).toContain('- [x] pending')
  } finally {
    cleanup()
  }
})

test('source mode reveals raw Markdown syntax and toggleInlineMark wraps and unwraps', () => {
  const { view, host, setSourceMode, cleanup } = makeEditor('# Title\n\nHello **world**')
  try {
    expect(host.querySelector('.cm-lp-h1')?.textContent).toBe('Title')
    setSourceMode(true)
    expect(host.querySelector('.cm-lp-h1')).toBeNull()
    expect(host.textContent).toContain('# Title')
    expect(host.textContent).toContain('**world**')

    // Wrap "Hello" in **bold**, then unwrap it
    view.dispatch({ selection: { anchor: 9, head: 14 } })
    toggleInlineMark(view.state, view.dispatch, '**')
    expect(view.state.doc.toString()).toBe('# Title\n\n**Hello** **world**')
    toggleInlineMark(view.state, view.dispatch, '**')
    expect(view.state.doc.toString()).toBe('# Title\n\nHello **world**')
  } finally {
    cleanup()
  }
})

test('focusing a line unfolds every Markdown mark on that line at once while other lines stay rendered', () => {
  const { view, host, cleanup } = makeEditor('Start of line with **bold** and [link](https://example.com)\nSecond **rendered** line', 0)
  try {
    view.focus()
    view.dispatch({ selection: { anchor: 0 } })
    const lines = host.querySelectorAll('.cm-line')
    // Cursor is at offset 0 (not touching **bold** or [link]), yet the entire first line shows raw Markdown
    expect(lines[0]?.textContent).toBe('Start of line with **bold** and [link](https://example.com)')
    // Meanwhile line 2 is rendered without **
    expect(lines[1]?.textContent).toBe('Second rendered line')
  } finally {
    cleanup()
  }
})
