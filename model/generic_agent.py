"""
Generic Visual Agent Model (Phase 0: L3 Monolithic)

Input:  pixels (4, 84, 84) float32 grayscale frame stack
Output: action token sequence (up to MAX_ACTION_TOKENS tokens)

Architecture: CNN encoder + Transformer decoder (autoregressive action generation)

The model has NO game-specific knowledge. It learns:
  - Which pixel patterns correspond to interactive elements
  - Causal relationships: "if I click here, the visual state changes this way"
  - Efficient action sequences: "move then click" vs "click directly"
"""
import torch
import torch.nn as nn
import torch.nn.functional as F
from typing import Tuple, Optional
from .action_space import ACTION_VOCAB_SIZE, MAX_ACTION_TOKENS, TOK_NOOP


class VisionEncoder(nn.Module):
    """CNN encoder: (B, 4, 84, 84) -> (B, N, d_model)"""

    def __init__(self, d_model: int = 256):
        super().__init__()
        # Nature CNN backbone, slightly modernized
        self.conv = nn.Sequential(
            nn.Conv2d(4, 32, 8, stride=4), nn.ReLU(),      # 84->20
            nn.Conv2d(32, 64, 4, stride=2), nn.ReLU(),     # 20->9
            nn.Conv2d(64, 64, 3, stride=1), nn.ReLU(),     # 9->7
        )
        # 64 channels * 7 * 7 = 3136 -> d_model
        self.fc = nn.Linear(64 * 7 * 7, d_model)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        # x: (B, 4, 84, 84)
        x = self.conv(x)                           # (B, 64, 7, 7)
        x = x.flatten(1)                           # (B, 3136)
        x = self.fc(x)                             # (B, d_model)
        return x.unsqueeze(1)                      # (B, 1, d_model) - single "image token"


class ActionDecoder(nn.Module):
    """Autoregressive transformer decoder: (B, 1, d_model) -> (B, max_tokens, vocab_size)"""

    def __init__(self, d_model: int = 256, nhead: int = 4, n_layers: int = 2,
                 max_tokens: int = MAX_ACTION_TOKENS):
        super().__init__()
        self.max_tokens = max_tokens
        self.d_model = d_model

        # Positional encoding for output tokens
        self.pos_embed = nn.Parameter(torch.randn(1, max_tokens, d_model) * 0.02)

        # Token embedding (for teacher forcing during training)
        self.token_embed = nn.Embedding(ACTION_VOCAB_SIZE, d_model)

        # Transformer decoder layers
        decoder_layer = nn.TransformerDecoderLayer(
            d_model=d_model, nhead=nhead, dim_feedforward=d_model * 2,
            dropout=0.1, batch_first=True
        )
        self.transformer = nn.TransformerDecoder(decoder_layer, num_layers=n_layers)

        # Output projection
        self.head = nn.Linear(d_model, ACTION_VOCAB_SIZE)

    def forward(self, memory: torch.Tensor,
                target_tokens: Optional[torch.Tensor] = None) -> torch.Tensor:
        """
        memory: (B, 1, d_model) from vision encoder
        target_tokens: (B, seq_len) for teacher forcing, or None for inference

        Returns: (B, max_tokens, vocab_size) logits
        """
        B = memory.shape[0]

        if target_tokens is not None:
            # Teacher forcing (training)
            seq_len = target_tokens.shape[1]
            tgt = self.token_embed(target_tokens)  # (B, seq_len, d_model)
            tgt = tgt + self.pos_embed[:, :seq_len, :]
            out = self.transformer(tgt, memory)    # (B, seq_len, d_model)
            return self.head(out)                   # (B, seq_len, vocab)
        else:
            # Autoregressive inference
            # Start with zero token (BOS)
            bos = torch.zeros(B, 1, dtype=torch.long, device=memory.device)
            tgt = self.token_embed(bos)             # (B, 1, d_model)
            tgt = tgt + self.pos_embed[:, :1, :]

            outputs = []
            for i in range(self.max_tokens):
                out = self.transformer(tgt, memory)  # (B, tgt_len, d_model)
                logits = self.head(out[:, -1:, :])   # (B, 1, vocab)
                outputs.append(logits)

                # Greedy next token
                next_token = logits.argmax(dim=-1)   # (B, 1)
                # Stop if all batch items output NOOP
                if (next_token == TOK_NOOP).all():
                    break

                next_emb = self.token_embed(next_token)
                next_emb = next_emb + self.pos_embed[:, i+1:i+2, :]
                tgt = torch.cat([tgt, next_emb], dim=1)

            return torch.cat(outputs, dim=1)  # (B, generated_len, vocab)


class GenericAgent(nn.Module):
    """
    Full generic agent model: pixels -> action tokens.

    Phase 0: Monolithic L3 model. No hierarchy, no router.
    Just learns to map visual input to action sequences.

    Training:
      - Supervised: (pixels, expert_actions) pairs from MLP self-play
      - RL: PPO with environment reward
    """

    def __init__(self, d_model: int = 256, nhead: int = 4, n_layers: int = 2):
        super().__init__()
        self.encoder = VisionEncoder(d_model)
        self.decoder = ActionDecoder(d_model, nhead, n_layers)

    def forward(self, pixels: torch.Tensor,
                target_tokens: Optional[torch.Tensor] = None) -> torch.Tensor:
        """
        pixels: (B, 4, 84, 84)
        target_tokens: (B, seq_len) for training, None for inference
        Returns: (B, seq_len, vocab_size) action logits
        """
        memory = self.encoder(pixels)
        return self.decoder(memory, target_tokens)

    @torch.no_grad()
    def act(self, pixels: torch.Tensor) -> torch.Tensor:
        """
        Inference: pixels -> greedy token sequence.
        Returns: (B, seq_len) token indices
        """
        logits = self.forward(pixels, target_tokens=None)
        return logits.argmax(dim=-1)

    def get_num_params(self) -> int:
        return sum(p.numel() for p in self.parameters())


def create_tictactoe_agent() -> GenericAgent:
    """
    Create a small agent suitable for TicTacToe PoC.
    ~0.8M parameters, <5ms inference on CPU.
    """
    return GenericAgent(d_model=128, nhead=4, n_layers=2)


def create_general_agent() -> GenericAgent:
    """
    Create a larger agent for general game playing.
    ~3M parameters, <10ms inference on GPU.
    """
    return GenericAgent(d_model=256, nhead=8, n_layers=4)
