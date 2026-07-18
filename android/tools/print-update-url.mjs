import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { dirname, join } from 'node:path'

const dir = dirname(fileURLToPath(import.meta.url))
const v = JSON.parse(readFileSync(join(dir, '..', 'version.json'), 'utf8'))
const base = String(v.download_base || '').replace(/\/?$/, '/')
console.log(`version: ${v.app}`)
console.log(`manifest: ${base}version.json`)
console.log(`apk: ${base}${v.apk}`)
