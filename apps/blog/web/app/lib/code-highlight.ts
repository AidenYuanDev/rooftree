// Lightweight client-side syntax highlighter for live-preview code blocks in the editor (Shiki runs
// only on the server at build time because its grammars are several megabytes)

const shellLangs = new Set([
  'sh',
  'bash',
  'zsh',
  'shell',
  'console',
  'ps1',
  'powershell',
  'pwsh',
  'yaml',
  'yml',
  'toml',
  'ini',
  'conf',
  'cmake',
  'dockerfile',
  'py',
  'python',
  'rb',
  'ruby',
])
const sqlLangs = new Set(['sql', 'mysql', 'postgresql', 'sqlite'])

const commonKeywords = new Set([
  'if',
  'else',
  'elif',
  'then',
  'fi',
  'for',
  'in',
  'do',
  'done',
  'while',
  'until',
  'case',
  'esac',
  'switch',
  'break',
  'continue',
  'return',
  'function',
  'fn',
  'func',
  'def',
  'class',
  'struct',
  'enum',
  'union',
  'namespace',
  'template',
  'typename',
  'interface',
  'type',
  'trait',
  'impl',
  'const',
  'let',
  'var',
  'mut',
  'auto',
  'static',
  'extern',
  'inline',
  'constexpr',
  'virtual',
  'override',
  'final',
  'noexcept',
  'explicit',
  'public',
  'private',
  'protected',
  'pub',
  'use',
  'mod',
  'import',
  'export',
  'from',
  'as',
  'default',
  'async',
  'await',
  'yield',
  'try',
  'catch',
  'throw',
  'new',
  'delete',
  'sizeof',
  'typeof',
  'void',
  'bool',
  'char',
  'short',
  'int',
  'long',
  'float',
  'double',
  'unsigned',
  'signed',
  'string',
  'true',
  'false',
  'null',
  'nullptr',
  'undefined',
  'None',
  'True',
  'False',
  'self',
  'Self',
  'this',
  'SELECT',
  'FROM',
  'WHERE',
  'INSERT',
  'INTO',
  'VALUES',
  'UPDATE',
  'SET',
  'DELETE',
  'CREATE',
  'TABLE',
  'INDEX',
  'DROP',
  'ALTER',
  'JOIN',
  'LEFT',
  'INNER',
  'ON',
  'AND',
  'OR',
  'NOT',
  'NULL',
  'ORDER',
  'BY',
  'LIMIT',
  'export',
  'local',
  'source',
  'alias',
  'unset',
  'sudo',
  'pacman',
  'apt',
  'dnf',
  'brew',
  'git',
  'docker',
  'pnpm',
  'npm',
  'cmake',
  'ninja',
  'curl',
  'sed',
  'awk',
  'grep',
  'rg',
  'echo',
  'cd',
  'mkdir',
  'rm',
  'cp',
  'mv',
  'chmod',
  'chown',
  'systemctl',
])

const cTokenRe =
  /(\/\/[^\n]*|\/\*[\s\S]*?\*\/)|("(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|`(?:\\.|[^`\\])*`)|(\b0x[\da-fA-F]+\b|\b\d+(?:\.\d+)?\b)|(\b[A-Za-z_]\w*\b)/g

const shellTokenRe =
  /(^|[ \t])(#[^\n]*)|("(?:\\.|[^"\\\n])*"|'[^'\n]*')|(\$\{?[A-Za-z_][\w:]*\}?)|((?:^|[ \t])--?[A-Za-z0-9][\w-]*)|(\b0x[\da-fA-F]+\b|\b\d+(?:\.\d+)?\b)|(\b[A-Za-z_][\w-]*\b)/gm

const sqlTokenRe = /(--[^\n]*|\/\*[\s\S]*?\*\/)|('(?:''|\\.|[^'\n])*'|"(?:\\.|[^"\n])*")|(\b\d+(?:\.\d+)?\b)|(\b[A-Za-z_]\w*\b)/g

function appendSpan(parent: Node, cls: string | null, text: string) {
  if (!text) return
  if (!cls) {
    parent.appendChild(document.createTextNode(text))
    return
  }
  const span = document.createElement('span')
  span.className = cls
  span.textContent = text
  parent.appendChild(span)
}

export function renderHighlightedLine(line: string, lang: string): DocumentFragment {
  const fragment = document.createDocumentFragment()
  if (!line) {
    fragment.appendChild(document.createTextNode('​'))
    return fragment
  }

  const normalized = lang.toLowerCase().trim()
  if (normalized === 'text' || normalized === 'plain' || normalized === 'plaintext') {
    fragment.appendChild(document.createTextNode(line))
    return fragment
  }

  if (shellLangs.has(normalized)) {
    let last = 0
    for (const match of line.matchAll(shellTokenRe)) {
      const index = match.index ?? 0
      if (index > last) appendSpan(fragment, null, line.slice(last, index))
      if (match[2] !== undefined) {
        appendSpan(fragment, null, match[1] ?? '')
        appendSpan(fragment, 'cm-tok-comment', match[2])
      } else if (match[3] !== undefined) {
        appendSpan(fragment, 'cm-tok-string', match[3])
      } else if (match[4] !== undefined) {
        appendSpan(fragment, 'cm-tok-var', match[4])
      } else if (match[5] !== undefined) {
        const raw = match[5]
        const lead = raw.startsWith(' ') || raw.startsWith('\t') ? raw[0]! : ''
        appendSpan(fragment, null, lead)
        appendSpan(fragment, 'cm-tok-flag', raw.slice(lead.length))
      } else if (match[6] !== undefined) {
        appendSpan(fragment, 'cm-tok-number', match[6])
      } else if (match[7] !== undefined) {
        appendSpan(fragment, commonKeywords.has(match[7]) ? 'cm-tok-keyword' : null, match[7])
      }
      last = index + match[0].length
    }
    if (last < line.length) appendSpan(fragment, null, line.slice(last))
    return fragment
  }

  const re = sqlLangs.has(normalized) ? sqlTokenRe : cTokenRe
  let last = 0
  for (const match of line.matchAll(re)) {
    const index = match.index ?? 0
    if (index > last) appendSpan(fragment, null, line.slice(last, index))
    if (match[1] !== undefined) {
      appendSpan(fragment, 'cm-tok-comment', match[1])
    } else if (match[2] !== undefined) {
      appendSpan(fragment, 'cm-tok-string', match[2])
    } else if (match[3] !== undefined) {
      appendSpan(fragment, 'cm-tok-number', match[3])
    } else if (match[4] !== undefined) {
      const word = match[4]
      const isKw = commonKeywords.has(word) || (sqlLangs.has(normalized) && commonKeywords.has(word.toUpperCase()))
      appendSpan(fragment, isKw ? 'cm-tok-keyword' : /^[A-Z]\w*$/.test(word) ? 'cm-tok-type' : null, word)
    }
    last = index + match[0].length
  }
  if (last < line.length) appendSpan(fragment, null, line.slice(last))
  return fragment
}
