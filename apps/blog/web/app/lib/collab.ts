import { collab, getSyncedVersion, receiveUpdates, sendableUpdates } from '@codemirror/collab'
import { ChangeSet, EditorState, type Extension, StateEffect, StateField, Transaction } from '@codemirror/state'
import { Decoration, type DecorationSet, EditorView, ViewPlugin, type ViewUpdate, WidgetType } from '@codemirror/view'

// A post's editors writing together (Collab.h on the server): the page's own WebSocket to /ws, each
// field ("post:12:zh:body") a document in a CodeMirror editor. Its changes go to the server on top of
// the version the editor has; the server's order is everyone's, the editor puts its own changes on
// top of what came first (@codemirror/collab). Where the others' cursors are is shown with their names

export interface PeerUser {
  id: number
  username: string
  displayName: string
  avatar: string | null
}

export interface Peer {
  client: string
  user: PeerUser
  selection: { anchor: number; head: number } | null
}

interface Update {
  changes: unknown
  client: string
}

// What the server sends about a document
export type DocMessage =
  | { op: 'state'; doc: string; version: number; text: string; peers: Peer[] }
  | { op: 'updates'; doc: string; version: number; updates: Update[]; peers?: Peer[] }
  | { op: 'rejected'; doc: string }
  | ({ op: 'peer'; doc: string } & Peer)
  | { op: 'left'; doc: string; client: string }
  | { op: 'closed'; doc: string; reason: 'removed' | 'replaced' | 'deleted' }
  | { op: 'error'; doc?: string; code: string }

interface DocHandler {
  receive: (message: DocMessage) => void
  // The version it has, to open again from after the connection was lost
  since: () => number | null
}

function randomId() {
  return Array.from(crypto.getRandomValues(new Uint8Array(12)), (byte) => byte.toString(36).padStart(2, '0'))
    .join('')
    .slice(0, 20)
}

// One per editing page: opens its documents again (from the version each has) when it reconnects
export class CollabConnection {
  readonly client = randomId()
  private socket: WebSocket | null = null
  private handlers = new Map<string, DocHandler>()
  private retry = 1000
  private disposed = false
  private onlineListeners = new Set<(online: boolean) => void>()

  constructor() {
    this.connect()
  }

  private connect() {
    const socket = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`)
    this.socket = socket
    socket.onopen = () => {
      this.retry = 1000
      for (const [doc, handler] of this.handlers) this.sendOpen(doc, handler.since())
      for (const listener of this.onlineListeners) listener(true)
    }
    socket.onmessage = (event: MessageEvent<string>) => {
      let message: { type?: string } & DocMessage
      try {
        message = JSON.parse(event.data) as typeof message
      } catch {
        return
      }
      if (message.type !== 'doc' || !message.doc) return
      this.handlers.get(message.doc)?.receive(message)
    }
    socket.onclose = () => {
      if (this.socket === socket) this.socket = null
      for (const listener of this.onlineListeners) listener(false)
      if (this.disposed) return
      setTimeout(() => this.connect(), this.retry)
      this.retry = Math.min(this.retry * 2, 15_000)
    }
  }

  get online() {
    return this.socket?.readyState === WebSocket.OPEN
  }

  onOnline(listener: (online: boolean) => void) {
    this.onlineListeners.add(listener)
    return () => void this.onlineListeners.delete(listener)
  }

  // Dropped while offline: opening again brings it up to date, and its changes are sent then
  send(message: Record<string, unknown>) {
    if (this.socket?.readyState === WebSocket.OPEN) this.socket.send(JSON.stringify({ type: 'doc', ...message }))
  }

  private sendOpen(doc: string, since: number | null) {
    this.send({ op: 'open', doc, client: this.client, ...(since === null ? {} : { since }) })
  }

  open(doc: string, handler: DocHandler) {
    this.handlers.set(doc, handler)
    this.sendOpen(doc, handler.since())
  }

  // Again from the start: its text as the server has it
  reopen(doc: string) {
    this.send({ op: 'close', doc })
    this.sendOpen(doc, null)
  }

  close(doc: string) {
    this.handlers.delete(doc)
    this.send({ op: 'close', doc })
  }

  dispose() {
    this.disposed = true
    this.socket?.close()
  }
}

// Each editor its colour, by its account
const colors = ['#e11d48', '#2563eb', '#16a34a', '#d97706', '#9333ea', '#0891b2', '#db2777', '#4f46e5']
export function peerColor(userId: number) {
  return colors[userId % colors.length]!
}

const setPeers = StateEffect.define<Peer[]>()

class CaretWidget extends WidgetType {
  constructor(
    readonly name: string,
    readonly color: string,
  ) {
    super()
  }
  eq(other: CaretWidget) {
    return other.name === this.name && other.color === this.color
  }
  toDOM() {
    const caret = document.createElement('span')
    caret.className = 'cm-peer-caret'
    caret.style.borderColor = this.color
    const label = document.createElement('span')
    label.className = 'cm-peer-label'
    label.style.backgroundColor = this.color
    // A CSS label (app.css): never text of the document, copied or read with it
    label.dataset.name = this.name
    caret.append(label)
    return caret
  }
  ignoreEvent() {
    return true
  }
}

// The others' selections, moved along with the text's changes
const peersField = StateField.define<{ peers: Peer[]; decorations: DecorationSet }>({
  create: () => ({ peers: [], decorations: Decoration.none }),
  update(value, tr) {
    let peers = value.peers
    if (tr.docChanged)
      peers = peers.map((peer) =>
        peer.selection
          ? { ...peer, selection: { anchor: tr.changes.mapPos(peer.selection.anchor), head: tr.changes.mapPos(peer.selection.head) } }
          : peer,
      )
    for (const effect of tr.effects) if (effect.is(setPeers)) peers = effect.value
    if (peers === value.peers && !tr.docChanged) return value

    const length = tr.state.doc.length
    const ranges = []
    for (const peer of peers) {
      if (!peer.selection) continue
      const color = peerColor(peer.user.id)
      const anchor = Math.min(peer.selection.anchor, length)
      const head = Math.min(peer.selection.head, length)
      if (anchor !== head)
        ranges.push(
          Decoration.mark({ attributes: { style: `background-color: ${color}33` } }).range(Math.min(anchor, head), Math.max(anchor, head)),
        )
      ranges.push(Decoration.widget({ widget: new CaretWidget(peer.user.displayName, color), side: 1 }).range(head))
    }
    return { peers, decorations: Decoration.set(ranges, true) }
  },
  provide: (field) => EditorView.decorations.from(field, (value) => value.decorations),
})

export interface DocEvents {
  onText?: (text: string) => void
  onPeers?: (peers: Peer[]) => void
  onClosed?: (reason: 'removed' | 'deleted') => void
  onError?: (code: string) => void
}

// A document in an editor made in parent once its text came: extensions are the editor's own (a
// Markdown one, a line). Returns what stops it
export function openDoc(connection: CollabConnection, doc: string, parent: HTMLElement, extensions: Extension, events: DocEvents) {
  let view: EditorView | null = null
  let pushing = false
  let peers: Peer[] = []
  let retry: ReturnType<typeof setTimeout> | undefined
  let cursorTimer: ReturnType<typeof setTimeout> | undefined

  const push = () => {
    if (!view || pushing || !connection.online) return
    const updates = sendableUpdates(view.state)
    if (!updates.length) return
    pushing = true
    connection.send({
      op: 'push',
      doc,
      version: getSyncedVersion(view.state),
      updates: updates.map((update) => ({ changes: update.changes.toJSON(), client: update.clientID })),
    })
  }

  const sendCursor = () => {
    clearTimeout(cursorTimer)
    cursorTimer = setTimeout(() => {
      if (!view) return
      const { anchor, head } = view.state.selection.main
      connection.send({ op: 'cursor', doc, selection: view.hasFocus ? { anchor, head } : null })
    }, 80)
  }

  const showPeers = (next: Peer[]) => {
    peers = next
    view?.dispatch({ effects: setPeers.of(peers) })
    events.onPeers?.(peers)
  }

  const sync = ViewPlugin.fromClass(
    class {
      update(update: ViewUpdate) {
        if (update.docChanged) {
          events.onText?.(update.state.doc.toString())
          push()
        }
        if (update.selectionSet || update.focusChanged) sendCursor()
      }
    },
  )

  const create = (text: string, version: number) => {
    const state = EditorState.create({
      doc: text,
      extensions: [extensions, collab({ startVersion: version, clientID: connection.client }), peersField, sync],
    })
    if (view) view.setState(state)
    else view = new EditorView({ state, parent })
    view.dispatch({ effects: setPeers.of(peers) })
    events.onText?.(text)
  }

  connection.open(doc, {
    since: () => (view ? getSyncedVersion(view.state) : null),
    receive: (message) => {
      switch (message.op) {
        case 'state':
          // The first time, or after it was away too long: what wasn't sent yet is lost
          pushing = false
          create(message.text, message.version)
          showPeers(message.peers)
          break
        case 'updates': {
          if (!view) break
          if (message.peers) showPeers(message.peers)
          if (message.version !== getSyncedVersion(view.state)) {
            // Missed some: from the start
            connection.reopen(doc)
            break
          }
          view.dispatch(
            receiveUpdates(
              view.state,
              message.updates.map((update) => ({ changes: ChangeSet.fromJSON(update.changes), clientID: update.client })),
            ),
          )
          pushing = false
          push()
          break
        }
        case 'rejected':
          // The newer changes are on their way; if they crossed, again in a moment
          pushing = false
          clearTimeout(retry)
          retry = setTimeout(push, 200)
          break
        case 'peer': {
          const peer: Peer = { client: message.client, user: message.user, selection: message.selection }
          showPeers([...peers.filter((item) => item.client !== peer.client), peer])
          break
        }
        case 'left':
          showPeers(peers.filter((item) => item.client !== message.client))
          break
        case 'closed':
          if (message.reason === 'replaced') connection.reopen(doc)
          else {
            view?.setState(EditorState.create({ doc: view.state.doc, extensions: [extensions, EditorState.readOnly.of(true)] }))
            events.onClosed?.(message.reason)
          }
          break
        case 'error':
          pushing = false
          events.onError?.(message.code)
          // A change it won't take: back to the server's text
          if (message.code === 'too_long' || message.code === 'invalid') connection.reopen(doc)
          break
      }
    },
  })

  const stopOnline = connection.onOnline((online) => {
    if (!online) pushing = false
  })

  return () => {
    clearTimeout(retry)
    clearTimeout(cursorTimer)
    stopOnline()
    connection.close(doc)
    view?.destroy()
    view = null
  }
}

// A title's editor: one line, Enter and pasted lines don't make another
export const singleLine: Extension = EditorState.transactionFilter.of((tr) =>
  tr.annotation(Transaction.userEvent) !== undefined && tr.newDoc.lines > 1 ? [] : tr,
)

// Inserts text at the selection of the editor in host (the image an upload made)
export function insertAtCursor(host: HTMLElement | null, text: string) {
  const view = host && EditorView.findFromDOM(host.querySelector('.cm-editor') as HTMLElement)
  if (!view) return false
  const { from, to } = view.state.selection.main
  view.dispatch({ changes: { from, to, insert: text }, selection: { anchor: from + text.length }, userEvent: 'input' })
  view.focus()
  return true
}
