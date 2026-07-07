"""View dumped test frames. Reads raw BGRA files from test/frames/."""
import struct, sys, os
from PIL import Image

def view_frame(path):
    with open(path, 'rb') as f:
        w = struct.unpack('<i', f.read(4))[0]
        h = struct.unpack('<i', f.read(4))[0]
        data = f.read()
    img = Image.frombytes('RGBA', (w, h), data, 'raw', 'BGRA')
    img.show()
    print(f"{path}: {w}x{h}")

if __name__ == '__main__':
    # Resolve test/frames relative to this script's location (project_root/test/)
    import os
    script_dir = os.path.dirname(os.path.abspath(__file__))
    d = os.path.join(script_dir, 'frames')
    if len(sys.argv) > 1:
        view_frame(sys.argv[1])
    else:
        files = sorted([f for f in os.listdir(d) if f.endswith('.bgra')])
        if files:
            view_frame(os.path.join(d, files[0]))
        else:
            print(f"No .bgra files in {d}/")
