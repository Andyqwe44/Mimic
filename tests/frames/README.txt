# Test frames directory
# Debug frame dumps (raw BGRA) are written here when debug_dump_frames is enabled.
# Each file: [w:4 LE][h:4 LE][pixels: w*h*4 BGRA]
# Open with: ffplay -f rawvideo -pixel_format bgra -video_size WxH -framerate 60 file.bgra
