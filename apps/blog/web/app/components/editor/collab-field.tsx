import { defaultKeymap, history, historyKeymap } from '@codemirror/commands'
import { markdown, markdownLanguage, pasteURLAsLink } from '@codemirror/lang-markdown'
import { Compartment, type Extension } from '@codemirror/state'
import { EditorView, keymap, placeholder as placeholderText } from '@codemirror/view'
import { useEffect, useRef, useState } from 'react'
import { Skeleton } from '~/components/ui/skeleton'
import { type CollabConnection, openDoc, type Peer, singleLine } from '~/lib/collab'
import { editorTheme, liveMarkdown, markdownShortcuts } from '~/lib/live-markdown'
import { cn } from '~/lib/utils'

// One field of a post's working copy, written together (lib/collab.ts): its title, summary or Markdown

export type FieldKind = 'line' | 'text' | 'markdown'

export function CollabField({
  connection,
  doc,
  kind,
  label,
  placeholder,
  lang,
  sourceMode = false,
  onToggleSource,
  className,
  onText,
  onPeers,
  onClosed,
  onError,
}: {
  connection: CollabConnection | null
  doc: string
  kind: FieldKind
  label: string
  placeholder?: string
  lang?: string
  // Markdown only: false renders Typora-style live preview, true shows raw Markdown source
  sourceMode?: boolean
  onToggleSource?: () => void
  className?: string
  onText?: (text: string) => void
  onPeers?: (doc: string, peers: Peer[]) => void
  onClosed?: (reason: 'removed' | 'deleted') => void
  onError?: (code: string) => void
}) {
  const host = useRef<HTMLDivElement>(null)
  const [liveCompartment] = useState(() => new Compartment())
  const [ready, setReady] = useState(false)
  // The latest callbacks, without opening the document again
  const events = useRef({ onText, onPeers, onClosed, onError, onToggleSource, sourceMode })
  useEffect(() => {
    events.current = { onText, onPeers, onClosed, onError, onToggleSource, sourceMode }
  })

  useEffect(() => {
    if (!connection || !host.current) return
    const common: Extension = [
      editorTheme,
      history(),
      keymap.of([...defaultKeymap, ...historyKeymap]),
      EditorView.contentAttributes.of({ 'aria-label': label }),
      placeholder ? placeholderText(placeholder) : [],
    ]
    const extensions: Extension =
      kind === 'line'
        ? [common, singleLine]
        : kind === 'text'
          ? [common, EditorView.lineWrapping]
          : [
              common,
              EditorView.lineWrapping,
              markdown({ base: markdownLanguage }),
              pasteURLAsLink,
              markdownShortcuts,
              keymap.of([
                {
                  key: 'Mod-/',
                  run: () => {
                    events.current.onToggleSource?.()
                    return true
                  },
                },
              ]),
              liveCompartment.of(events.current.sourceMode ? [] : liveMarkdown()),
            ]

    const stop = openDoc(connection, doc, host.current, extensions, {
      onText: (text) => {
        setReady(true)
        events.current.onText?.(text)
      },
      onPeers: (peers) => events.current.onPeers?.(doc, peers),
      onClosed: (reason) => events.current.onClosed?.(reason),
      onError: (code) => events.current.onError?.(code),
    })
    return () => {
      stop()
      events.current.onPeers?.(doc, [])
    }
    // The label and the hint are the editor's from its start
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [connection, doc, kind])

  useEffect(() => {
    if (kind !== 'markdown' || !ready) return
    const cm = host.current?.querySelector('.cm-editor')
    const view = cm ? EditorView.findFromDOM(cm as HTMLElement) : null
    view?.dispatch({ effects: liveCompartment.reconfigure(sourceMode ? [] : liveMarkdown()) })
  }, [kind, ready, sourceMode, liveCompartment])

  return (
    <div
      lang={lang}
      className={cn(
        'relative rounded-md border border-input bg-background/60 shadow-xs focus-within:border-ring focus-within:ring-[3px] focus-within:ring-ring/50',
        kind === 'markdown'
          ? sourceMode
            ? 'font-mono text-sm leading-relaxed'
            : 'text-base leading-relaxed'
          : !className?.match(/\btext-(xs|sm|base|lg|xl|\dxl)\b/) && 'text-sm leading-relaxed',
        className,
      )}
    >
      {!ready && <Skeleton className={cn('m-2', kind === 'markdown' ? 'h-64' : 'h-5')} />}
      <div ref={host} className={cn(!ready && 'hidden')} />
    </div>
  )
}
