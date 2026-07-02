"""
Generic Action Space

Maps between the model's discrete token vocabulary and physical input actions.
Game-agnostic: no game-specific semantics. Pure "move mouse here, click there, press key X".

Token vocabulary (compact representation for model autoregressive output):
  Each action = 1 header token + parameters as subsequent tokens

  MOUSE_MOVE_ABS:  [0] [x_norm] [y_norm]           = 3 tokens (x,y in 0..1 normalized)
  MOUSE_MOVE_REL:  [1] [dx] [dy]                    = 3 tokens (pixel deltas, discretized)
  MOUSE_CLICK:     [2] [x_norm] [y_norm] [btn_idx]  = 4 tokens
  MOUSE_DOWN:      [3] [btn_idx]                    = 2 tokens
  MOUSE_UP:        [4] [btn_idx]                    = 2 tokens
  KEY_PRESS:       [5] [vk_code]                    = 2 tokens
  KEY_RELEASE:     [6] [vk_code]                    = 2 tokens
  KEY_TAP:         [7] [vk_code] [duration_ms]      = 3 tokens
  WAIT:            [8] [ms]                         = 2 tokens
  NOOP:            [255]                            = 1 token (padding/end)
"""
from dataclasses import dataclass
from typing import List, Optional, Tuple
import struct

# Token type values
TOK_MOUSE_MOVE_ABS = 0
TOK_MOUSE_MOVE_REL = 1
TOK_MOUSE_CLICK    = 2
TOK_MOUSE_DOWN     = 3
TOK_MOUSE_UP       = 4
TOK_KEY_PRESS      = 5
TOK_KEY_RELEASE    = 6
TOK_KEY_TAP        = 7
TOK_WAIT           = 8
TOK_NOOP           = 255

# Vocabulary size for the model's output head
ACTION_VOCAB_SIZE = 256  # 0-255 token types

# Max tokens per action sequence (model outputs fixed-length, truncated at NOOP)
MAX_ACTION_TOKENS = 32

# Button indices
BTN_LEFT   = 0
BTN_RIGHT  = 1
BTN_MIDDLE = 2

# Common virtual key codes
VK_SPACE  = 0x20
VK_RETURN = 0x0D
VK_ESCAPE = 0x1B
VK_TAB    = 0x09
VK_LEFT   = 0x25
VK_UP     = 0x26
VK_RIGHT  = 0x27
VK_DOWN   = 0x28
VK_W      = 0x57
VK_A      = 0x41
VK_S      = 0x53
VK_D      = 0x44


@dataclass
class ParsedAction:
    """A single parsed action ready for execution"""
    type: int
    x: float = 0.0       # normalized 0..1 (absolute)
    y: float = 0.0
    dx: int = 0          # pixel delta (relative)
    dy: int = 0
    btn: int = 0         # 0=left, 1=right, 2=middle
    vk_code: int = 0
    duration_ms: int = 0


def tokens_to_actions(tokens: List[int]) -> List[ParsedAction]:
    """
    Decode model output token sequence into parsed actions.
    Stops at first NOOP token.
    """
    actions = []
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if tok == TOK_NOOP:
            break

        if tok == TOK_MOUSE_MOVE_ABS and i + 2 < len(tokens):
            actions.append(ParsedAction(type=tok, x=tokens[i+1], y=tokens[i+2]))
            i += 3
        elif tok == TOK_MOUSE_MOVE_REL and i + 2 < len(tokens):
            actions.append(ParsedAction(type=tok, dx=tokens[i+1], dy=tokens[i+2]))
            i += 3
        elif tok == TOK_MOUSE_CLICK and i + 3 < len(tokens):
            actions.append(ParsedAction(type=tok, x=tokens[i+1], y=tokens[i+2], btn=tokens[i+3]))
            i += 4
        elif tok in (TOK_MOUSE_DOWN, TOK_MOUSE_UP) and i + 1 < len(tokens):
            actions.append(ParsedAction(type=tok, btn=tokens[i+1]))
            i += 2
        elif tok in (TOK_KEY_PRESS, TOK_KEY_RELEASE) and i + 1 < len(tokens):
            actions.append(ParsedAction(type=tok, vk_code=tokens[i+1]))
            i += 2
        elif tok == TOK_KEY_TAP and i + 2 < len(tokens):
            actions.append(ParsedAction(type=tok, vk_code=tokens[i+1], duration_ms=tokens[i+2]))
            i += 3
        elif tok == TOK_WAIT and i + 1 < len(tokens):
            actions.append(ParsedAction(type=tok, duration_ms=tokens[i+1]))
            i += 2
        else:
            i += 1  # malformed, skip
    return actions


def actions_to_bytes(actions: List[ParsedAction]) -> bytes:
    """Serialize actions to C++ wire format"""
    buf = bytearray()
    for a in actions:
        buf.append(a.type & 0xFF)
        if a.type == TOK_MOUSE_MOVE_ABS:
            buf += struct.pack('<ff', a.x, a.y)
        elif a.type == TOK_MOUSE_MOVE_REL:
            buf += struct.pack('<ii', a.dx, a.dy)
        elif a.type == TOK_MOUSE_CLICK:
            buf += struct.pack('<ffB', a.x, a.y, a.btn)
        elif a.type in (TOK_MOUSE_DOWN, TOK_MOUSE_UP):
            buf.append(a.btn & 0xFF)
        elif a.type in (TOK_KEY_PRESS, TOK_KEY_RELEASE):
            buf += struct.pack('>H', a.vk_code)
        elif a.type == TOK_KEY_TAP:
            buf += struct.pack('>Hi', a.vk_code, a.duration_ms)
        elif a.type == TOK_WAIT:
            buf += struct.pack('<i', a.duration_ms)
    buf.append(TOK_NOOP)
    return bytes(buf)


