// ═══ useScrollLock — prevent background scroll when modal is open ═══
import { useEffect } from 'react'

/**
 * Lock body scroll when active (default: always locked on mount).
 * Pass `active` for components that are always mounted but conditionally visible.
 */
export function useScrollLock(active = true) {
  useEffect(() => {
    if (!active) return
    const prev = document.body.style.overflow
    document.body.style.overflow = 'hidden'
    return () => {
      document.body.style.overflow = prev
    }
  }, [active])
}
