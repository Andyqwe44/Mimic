"""make_app_icon.py — (re)generate monitor_app/app.ico from a source logo image.

Usage:
    python tools/make_app_icon.py <source_logo.png>
    python tools/make_app_icon.py            # re-tighten the current app.ico

WHY THIS EXISTS (read before swapping the logo):
  The icon Windows shows in the TASKBAR and in Explorer for the exe is the exe's
  RESOURCE icon — app.rc (`IDI_APPICON ICON "app.ico"`) compiled into the exe —
  NOT the window HICON set in main.cpp's WNDCLASS. Changing the window icon does
  nothing for the taskbar/Explorer. So to change the app's visible icon you edit
  app.ico and rebuild.

  The icon looked "a size smaller than its peers" because the logo only filled
  ~60% of the .ico canvas (big transparent margins). This script crops to the
  logo's actual pixels, re-centres it with a small margin so it fills ~90% of the
  canvas, and emits every size the shell needs — including 24px, the native
  Windows 10/11 taskbar size.

AFTER RUNNING:
  1. Rebuild:  powershell -File scripts\\Build.ps1 -Module monitor_app
     (rc.exe re-embeds the new app.ico into monitor_app.exe)
  2. Clear the Windows icon cache so Explorer stops showing the old cached icon:
       ie4uinit.exe -show
     or, if that's not enough:
       taskkill /f /im explorer.exe
       Remove-Item "$env:LOCALAPPDATA\\Microsoft\\Windows\\Explorer\\iconcache*.db" -Force -EA 0
       Start-Process explorer

NOTE: give a high-resolution source (>=256px) if you want a crisp 256px frame —
  PIL won't upscale a small canvas to 256, so a small source omits the 256 frame
  (fine for taskbar/title-bar which use <=48px).
"""
import sys
from PIL import Image

src = sys.argv[1] if len(sys.argv) > 1 else 'monitor_app/app.ico'
out = 'monitor_app/app.ico'
MARGIN = 0.05                                   # ~5% breathing room around the logo
SIZES = [16, 20, 24, 32, 48, 64, 128, 256]      # 24 = Win10/11 taskbar native size

im = Image.open(src).convert('RGBA')
bb = im.getbbox()
if not bb:
    sys.exit('source image is empty / fully transparent')
logo = im.crop(bb)                              # tight crop -> removes padding
lw, lh = logo.size
side = max(lw, lh)
margin = round(side * MARGIN)
cs = side + 2 * margin
canvas = Image.new('RGBA', (cs, cs), (0, 0, 0, 0))
canvas.paste(logo, ((cs - lw) // 2, (cs - lh) // 2), logo)
canvas.save(out, format='ICO', sizes=[(s, s) for s in SIZES])
print(f'wrote {out}: logo {lw}x{lh} -> {cs}x{cs} canvas, sizes {SIZES}')
