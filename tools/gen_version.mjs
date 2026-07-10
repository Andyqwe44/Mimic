/**
 * gen_version.mjs — Generate version.json manifest for release directory.
 *
 * Usage: node tools/gen_version.mjs <release_dir> <version>
 *
 * Walks all files in release_dir, computes SHA256, writes version.json.
 */
import { readFileSync, writeFileSync } from 'fs'
import { createHash } from 'crypto'
import { join, relative } from 'path'
import { readdirSync, statSync } from 'fs'

function walk(dir, base) {
    const files = {}
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
        const full = join(dir, entry.name)
        const rel = relative(base, full).replace(/\\/g, '/')
        if (entry.isDirectory()) {
            Object.assign(files, walk(full, base))
        } else {
            const data = readFileSync(full)
            const sha256 = createHash('sha256').update(data).digest('hex')
            files[rel] = { sha256, size: data.length }
        }
    }
    return files
}

const [releaseDir, version] = process.argv.slice(2)
if (!releaseDir || !version) {
    console.error('Usage: node tools/gen_version.mjs <release_dir> <version>')
    process.exit(1)
}

const files = walk(releaseDir, releaseDir)

const manifest = {
    app: version,
    released: new Date().toISOString(),
    files: {}
}

for (const [path, info] of Object.entries(files).sort()) {
    manifest.files[path] = {
        v: version,
        sha256: info.sha256,
        size: info.size
    }
}

const json = JSON.stringify(manifest, null, 2)
const outPath = join(releaseDir, 'version.json')
writeFileSync(outPath, json, 'utf-8')

console.log(`version.json written (${Object.keys(files).length} files, ${json.length} bytes)`)
