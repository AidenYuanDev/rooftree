import { defaultKeymap, history, historyKeymap } from '@codemirror/commands'
import { markdown, markdownLanguage, pasteURLAsLink } from '@codemirror/lang-markdown'
import { Compartment, EditorState } from '@codemirror/state'
import { EditorView, keymap, placeholder as placeholderText } from '@codemirror/view'
import { useEffect, useRef, useState } from 'react'
import { editorTheme, liveMarkdown, markdownShortcuts } from '~/lib/live-markdown'
import { cn } from '~/lib/utils'

// A standalone Markdown editor with Typora-style live preview (for new posts before they have a
// collaborative id, and for notes)
export function MarkdownField({
  id,
  value,
  onChange,
  label,
  placeholder,
  lang,
  sourceMode = false,
  onToggleSource,
  invalid,
  hostRef,
  className,
}: {
  id?: string
  value: string
  onChange: (value: string) => void
  label: string
  placeholder?: string
  lang?: string
  sourceMode?: boolean
  onToggleSource?: () => void
  invalid?: boolean
  hostRef?: React.RefObject<HTMLDivElement | null>
  className?: string
}) {
  const internalHost = useRef<HTMLDivElement>(null)
  const host = hostRef ?? internalHost
  const viewRef = useRef<EditorView | null>(null)
  const [liveCompartment] = useState(() => new Compartment())
  const callbacks = useRef({ onChange, onToggleSource, sourceMode, value })
  useEffect(() => {
    callbacks.current = { onChange, onToggleSource, sourceMode, value }
  })

  useEffect(() => {
    if (!host.current) return
    const attributes: Record<string, string> = { 'aria-label': label }
    if (id) attributes.id = id
    if (invalid) attributes['aria-invalid'] = 'true'
    const state = EditorState.create({
      doc: callbacks.current.value,
      extensions: [
        editorTheme,
        history(),
        keymap.of([...defaultKeymap, ...historyKeymap]),
        EditorView.lineWrapping,
        EditorView.contentAttributes.of(attributes),
        placeholder ? placeholderText(placeholder) : [],
        markdown({ base: markdownLanguage }),
        pasteURLAsLink,
        markdownShortcuts,
        keymap.of([
          {
            key: 'Mod-/',
            run: () => {
              callbacks.current.onToggleSource?.()
              return true
            },
          },
        ]),
        liveCompartment.of(callbacks.current.sourceMode ? [] : liveMarkdown()),
        EditorView.updateListener.of((update) => {
          if (update.docChanged) callbacks.current.onChange(update.state.doc.toString())
        }),
      ],
    })
    const view = new EditorView({ state, parent: host.current })
    viewRef.current = view
    return () => {
      view.destroy()
      viewRef.current = null
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [id, label, placeholder, invalid])

  useEffect(() => {
    const view = viewRef.current
    if (!view) return
    const current = view.state.doc.toString()
    if (value !== current) view.dispatch({ changes: { from: 0, to: current.length, insert: value } })
  }, [value])

  useEffect(() => {
    viewRef.current?.dispatch({ effects: liveCompartment.reconfigure(sourceMode ? [] : liveMarkdown()) })
  }, [sourceMode, liveCompartment])

  return (
    <div
      ref={host}
      lang={lang}
      className={cn(
        'min-h-[24rem] rounded-md border border-input bg-background/60 shadow-xs focus-within:border-ring focus-within:ring-[3px] focus-within:ring-ring/50',
        sourceMode ? 'font-mono text-sm leading-relaxed' : 'text-base leading-relaxed',
        className,
      )}
    />
  )
}
