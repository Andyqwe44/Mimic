import { useEffect, useState } from 'react'
import { BP } from '../lib/design'

export type ShellMode = 'side' | 'side-icon' | 'bottom'

export function useViewport() {
  const [width, setWidth] = useState(
    () => (typeof window !== 'undefined' ? window.innerWidth : BP.desktop),
  )
  const [height, setHeight] = useState(
    () => (typeof window !== 'undefined' ? window.innerHeight : 800),
  )

  useEffect(() => {
    const onResize = () => {
      setWidth(window.innerWidth)
      setHeight(window.innerHeight)
    }
    window.addEventListener('resize', onResize)
    onResize()
    return () => window.removeEventListener('resize', onResize)
  }, [])

  const shellMode: ShellMode =
    width >= BP.desktop ? 'side' : width >= BP.tablet ? 'side-icon' : 'bottom'
  const isNarrow = width < BP.narrow
  const isShort = height < BP.short

  return { width, height, shellMode, isNarrow, isShort }
}
