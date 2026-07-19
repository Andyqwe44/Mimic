// In-app letter keyboard — sends keydown/keyup so the controlled host IME decides layout.
import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { SOFT_KB, TEXT } from '../lib/design'

const ROW1 = ['q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p']
const ROW2 = ['a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l']
const ROW3 = ['z', 'x', 'c', 'v', 'b', 'n', 'm']

function keyCodeFor(letter: string) {
  const u = letter.toUpperCase()
  return { key: letter, code: `Key${u}` }
}

function KeyBtn({
  label,
  wide,
  active,
  onPress,
}: {
  label: string
  wide?: boolean
  active?: boolean
  onPress: () => void
}) {
  return (
    <button
      type="button"
      className={`${wide ? 'flex-[1.6]' : 'flex-1'} ${SOFT_KB.keyH} min-w-0 rounded-md ${TEXT.sm} font-medium transition-colors ${
        active
          ? 'bg-accent-soft-mid text-accent ring-1 ring-accent-ring'
          : 'bg-bg-tertiary text-text-primary active:bg-accent-soft'
      }`}
      onClick={onPress}
    >
      {label}
    </button>
  )
}

export function SoftKeyboardOverlay({
  open,
  onClose,
  onKey,
}: {
  open: boolean
  onClose: () => void
  onKey: (type: 'keydown' | 'keyup', key: string, code: string) => void
}) {
  const { t } = useTranslation()
  const [shift, setShift] = useState(false)
  const [ctrl, setCtrl] = useState(false)
  const [alt, setAlt] = useState(false)

  if (!open) return null

  const tapLetter = (ch: string) => {
    const letter = shift ? ch.toUpperCase() : ch.toLowerCase()
    const { key, code } = keyCodeFor(letter.toLowerCase())
    const sendKey = shift ? letter : key
    if (ctrl) onKey('keydown', 'Control', 'ControlLeft')
    if (alt) onKey('keydown', 'Alt', 'AltLeft')
    if (shift) onKey('keydown', 'Shift', 'ShiftLeft')
    onKey('keydown', sendKey, code)
    onKey('keyup', sendKey, code)
    if (shift) onKey('keyup', 'Shift', 'ShiftLeft')
    if (alt) onKey('keyup', 'Alt', 'AltLeft')
    if (ctrl) onKey('keyup', 'Control', 'ControlLeft')
    if (shift) setShift(false)
  }

  const tapSpecial = (key: string, code: string) => {
    onKey('keydown', key, code)
    onKey('keyup', key, code)
  }

  return (
    <div
      className={`w-full bg-bg-secondary/95 backdrop-blur-sm border-t border-border ${SOFT_KB.pad} ${SOFT_KB.spaceY} pointer-events-auto`}
      data-no-page-swipe
      style={{ paddingBottom: 'max(0.625rem, env(safe-area-inset-bottom, 0px))' }}
    >
      <div className="flex items-center justify-between px-1">
        <span className={`${TEXT.xs} text-text-secondary`}>{t('peer.soft_kb_title')}</span>
        <button
          type="button"
          className={`${TEXT.xs} text-accent px-2 py-1`}
          onClick={onClose}
        >
          {t('peer.soft_kb_close')}
        </button>
      </div>
      <div className={`flex ${SOFT_KB.gap}`}>
        {ROW1.map((c) => (
          <KeyBtn key={c} label={shift ? c.toUpperCase() : c} onPress={() => tapLetter(c)} />
        ))}
      </div>
      <div className={`flex ${SOFT_KB.gap} px-3`}>
        {ROW2.map((c) => (
          <KeyBtn key={c} label={shift ? c.toUpperCase() : c} onPress={() => tapLetter(c)} />
        ))}
      </div>
      <div className={`flex ${SOFT_KB.gap}`}>
        <KeyBtn label="⇧" wide active={shift} onPress={() => setShift((v) => !v)} />
        {ROW3.map((c) => (
          <KeyBtn key={c} label={shift ? c.toUpperCase() : c} onPress={() => tapLetter(c)} />
        ))}
        <KeyBtn label="⌫" wide onPress={() => tapSpecial('Backspace', 'Backspace')} />
      </div>
      <div className={`flex ${SOFT_KB.gap}`}>
        <KeyBtn label="Ctrl" wide active={ctrl} onPress={() => setCtrl((v) => !v)} />
        <KeyBtn label="Alt" wide active={alt} onPress={() => setAlt((v) => !v)} />
        <KeyBtn label="Tab" onPress={() => tapSpecial('Tab', 'Tab')} />
        <KeyBtn label="Esc" onPress={() => tapSpecial('Escape', 'Escape')} />
        <KeyBtn label="↵" wide onPress={() => tapSpecial('Enter', 'Enter')} />
        <KeyBtn label="␣" wide onPress={() => tapSpecial(' ', 'Space')} />
      </div>
      <p className={`${TEXT.tiny} text-text-muted px-1`}>{t('peer.soft_kb_hint')}</p>
    </div>
  )
}
