#!/usr/bin/env node
// Copies the blog's data from its old MySQL database into PostgreSQL, once, when moving a deployment:
//
//   DATABASE_URL=postgres://blog:...@127.0.0.1:5432/rooftree_blog?sslmode=disable \
//   MYSQL_URL=mysql://blog:...@127.0.0.1:3306/<the old database> \
//   node apps/blog/deploy/migrate-from-mysql.mjs
//
// The target has the schema (dbmate up) and no rows; it refuses otherwise. It reads MySQL only. Needs the
// mysql and psql clients, and a superuser on PostgreSQL (POSTGRES_USER of the container is one): the
// foreign keys are off during the copy, a folder may have a parent with a larger id. All of it runs in
// one transaction, the row counts of both sides are compared before it commits.

import { spawn, execFileSync } from 'node:child_process'
import { mkdtempSync, rmSync, createWriteStream } from 'node:fs'
import { once } from 'node:events'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { createInterface } from 'node:readline'

const mysqlUrl = new URL(process.env.MYSQL_URL ?? '')
const postgresUrl = process.env.DATABASE_URL ?? ''
if (!postgresUrl.startsWith('postgres'))
  throw new Error('DATABASE_URL is not a PostgreSQL URL')

const mysqlArgs = ['--host', mysqlUrl.hostname, '--port', mysqlUrl.port || '3306', '--user', decodeURIComponent(mysqlUrl.username),
  '--default-character-set=utf8mb4', '--batch', '--raw', '--skip-column-names', '--max_allowed_packet=1G', mysqlUrl.pathname.slice(1)]
const mysqlEnv = { ...process.env, MYSQL_PWD: decodeURIComponent(mysqlUrl.password) }

function mysql(sql) {
  return execFileSync('mysql', [...mysqlArgs, '-e', sql], { env: mysqlEnv, encoding: 'utf8', maxBuffer: 1 << 28 })
}

// libpq's URL with ?sslmode, which psql takes as it is
function psql(sql) {
  return execFileSync('psql', [postgresUrl, '-X', '-At', '-v', 'ON_ERROR_STOP=1', '-c', sql], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'inherit'] }).trim()
}

const NewTableName = { user: 'users' }

const tables = mysql(`SELECT table_name FROM information_schema.tables WHERE table_schema = DATABASE() AND table_type = 'BASE TABLE' AND table_name <> 'schema_migrations' ORDER BY table_name`)
  .trim().split('\n')

const targets = psql(`SELECT table_name FROM information_schema.tables WHERE table_schema = 'public' AND table_name <> 'schema_migrations'`).split('\n')
for (const table of tables)
  if (!targets.includes(NewTableName[table] ?? table))
    throw new Error(`MySQL's table ${table} has no place in PostgreSQL: is the old database at its last migration, and dbmate up done?`)

for (const target of targets)
  if (psql(`SELECT EXISTS (SELECT 1 FROM ${target})`) === 't')
    throw new Error(`${target} has rows already, this is for an empty database`)

const dir = mkdtempSync(join(tmpdir(), 'blog-migrate-'))
const script = ['\\set ON_ERROR_STOP on', 'BEGIN;', 'SET session_replication_role = replica;']
const expected = {}

// A CSV field: NULL is nothing at all, every text is quoted so an empty one stays empty
function csv(value) {
  if (value === null)
    return ''
  const text = typeof value === 'string' ? value : JSON.stringify(value)
  return `"${text.replaceAll('"', '""')}"`
}

for (const table of tables) {
  const target = NewTableName[table] ?? table
  // Generated columns follow by themselves
  const columns = mysql(`SELECT column_name, data_type FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = '${table}' AND generation_expression = '' ORDER BY ordinal_position`)
    .trim().split('\n').map((line) => line.split('\t'))

  // One row a line as JSON: no doubt about NULL, tabs or newlines in a text. Bytes as hex, for bytea
  const values = columns.map(([name, type]) => (/binary|blob/.test(type) ? `CONCAT('\\\\x', HEX(\`${name}\`))` : `\`${name}\``))
  const select = `SELECT JSON_ARRAY(${values.join(', ')}) FROM \`${table}\``
  const child = spawn('mysql', [...mysqlArgs, '-e', select], { env: mysqlEnv, stdio: ['ignore', 'pipe', 'inherit'] })
  const file = join(dir, `${target}.csv`)
  const out = createWriteStream(file)
  let rows = 0
  for await (const line of createInterface({ input: child.stdout, crlfDelay: Infinity })) {
    const row = JSON.parse(line)
    if (!out.write(row.map(csv).join(',') + '\n'))
      await once(out, 'drain')
    ++rows
  }
  out.end()
  await once(out, 'finish')
  const [code] = child.exitCode === null ? await once(child, 'exit') : [child.exitCode]
  if (code !== 0)
    throw new Error(`mysql failed on ${table}`)

  expected[target] = rows
  script.push(`\\copy ${target} (${columns.map(([name]) => `"${name}"`).join(', ')}) FROM '${file}' WITH (FORMAT csv)`)
  console.log(`${table}: ${rows} rows`)
}

script.push('SET session_replication_role = origin;')

// Where each id goes on from
const identities = psql(`SELECT table_name FROM information_schema.columns WHERE table_schema = 'public' AND column_name = 'id' AND is_identity = 'YES'`).split('\n')
for (const target of identities)
  script.push(`SELECT setval(pg_get_serial_sequence('${target}', 'id'), GREATEST((SELECT MAX(id) FROM ${target}), 1));`)

// Nothing committed unless every table has its rows and every foreign key holds
for (const [target, rows] of Object.entries(expected))
  script.push(`DO $$ BEGIN IF (SELECT COUNT(*) FROM ${target}) <> ${rows} THEN RAISE EXCEPTION '${target} has not its ${rows} rows'; END IF; END $$;`)
const foreignKeys = psql(`SELECT conrelid::regclass || '|' || conname || '|' || pg_get_constraintdef(oid) FROM pg_constraint WHERE contype = 'f' AND connamespace = 'public'::regnamespace`).split('\n')
for (const key of foreignKeys) {
  const [table, name, definition] = key.split('|')
  const match = /FOREIGN KEY \((.+?)\) REFERENCES (\w+)\((.+?)\)/.exec(definition)
  script.push(`DO $$ BEGIN IF EXISTS (SELECT 1 FROM ${table} c WHERE c.${match[1]} IS NOT NULL AND NOT EXISTS (SELECT 1 FROM ${match[2]} p WHERE p.${match[3]} = c.${match[1]})) THEN RAISE EXCEPTION '${name} does not hold'; END IF; END $$;`)
}

script.push('COMMIT;')
execFileSync('psql', [postgresUrl, '-X', '-q', '-f', '-'], { input: script.join('\n'), stdio: ['pipe', 'ignore', 'inherit'] })
rmSync(dir, { recursive: true })
console.log('Copied, every row count and foreign key checked.')
