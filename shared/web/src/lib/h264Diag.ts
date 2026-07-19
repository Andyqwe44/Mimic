/** Annex-B H.264 helpers for decode-path diagnostics (tearing / missing IDR). */

export type NalInfo = { type: number; offset: number; length: number }

/** Scan Annex-B for NAL units (3- or 4-byte start codes). */
export function scanAnnexB(u8: Uint8Array): NalInfo[] {
  const out: NalInfo[] = []
  let i = 0
  while (i + 3 < u8.length) {
    let sc = 0
    if (u8[i] === 0 && u8[i + 1] === 0 && u8[i + 2] === 0 && u8[i + 3] === 1) sc = 4
    else if (u8[i] === 0 && u8[i + 1] === 0 && u8[i + 2] === 1) sc = 3
    if (sc === 0) {
      i++
      continue
    }
    const nalStart = i + sc
    if (nalStart >= u8.length) break
    let j = nalStart
    while (j + 3 < u8.length) {
      if (u8[j] === 0 && u8[j + 1] === 0 && (u8[j + 2] === 1 || (u8[j + 2] === 0 && u8[j + 3] === 1))) break
      j++
    }
    if (j + 3 >= u8.length) j = u8.length
    out.push({ type: u8[nalStart] & 0x1f, offset: nalStart, length: j - nalStart })
    i = j
  }
  return out
}

export function annexbHasIdr(u8: Uint8Array): boolean {
  return scanAnnexB(u8).some((n) => n.type === 5)
}

export function annexbHasSps(u8: Uint8Array): boolean {
  return scanAnnexB(u8).some((n) => n.type === 7)
}

export function annexbHasPps(u8: Uint8Array): boolean {
  return scanAnnexB(u8).some((n) => n.type === 8)
}

/** Parse profile_idc / level_idc from first SPS NAL → `avc1.PPCCLL` (Baseline constraint=0x00). */
export function parseSpsCodecString(u8: Uint8Array): string | null {
  const nals = scanAnnexB(u8)
  const sps = nals.find((n) => n.type === 7)
  if (!sps || sps.length < 4) return null
  const profile = u8[sps.offset + 1]
  const constraints = u8[sps.offset + 2]
  const level = u8[sps.offset + 3]
  const pp = profile.toString(16).padStart(2, '0').toUpperCase()
  const cc = constraints.toString(16).padStart(2, '0').toUpperCase()
  const ll = level.toString(16).padStart(2, '0').toUpperCase()
  return `avc1.${pp}${cc}${ll}`
}

export function profileName(profileIdc: number): string {
  switch (profileIdc) {
    case 66: return 'Baseline'
    case 77: return 'Main'
    case 88: return 'Extended'
    case 100: return 'High'
    default: return `P${profileIdc}`
  }
}

/** Human-readable SPS summary from Annex-B. */
export function summarizeSps(u8: Uint8Array): { codec: string; profile: string; level: number } | null {
  const codec = parseSpsCodecString(u8)
  if (!codec) return null
  const nals = scanAnnexB(u8)
  const sps = nals.find((n) => n.type === 7)
  if (!sps || sps.length < 4) return null
  const profileIdc = u8[sps.offset + 1]
  const levelIdc = u8[sps.offset + 3]
  return { codec, profile: profileName(profileIdc), level: levelIdc / 10 }
}
