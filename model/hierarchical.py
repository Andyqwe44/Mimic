"""
Hierarchical Model: L1 Perception Specialist + L2 Strategic Reasoner

L1 (Perception): 4x84x84 -> Conv -> 16-dim z
  - Small CNN that compresses visual input into a compact representation
  - z is NOT predefined by humans — it's learned through information bottleneck
  - The model invents its own "language" for describing visual state

L2 (Reasoner): z(16) + history(8) -> MLP -> action tokens
  - Strategic decision-making from compressed visual representation
  - plus action history embedding for temporal context

Training: end-to-end with information bottleneck loss:
  L = L_task + beta * L_recon + gamma * ||z||_2
"""
import torch
import torch.nn as nn
import torch.nn.functional as F
from typing import Tuple, Optional, Dict
from .action_space import ACTION_VOCAB_SIZE, MAX_ACTION_TOKENS, TOK_NOOP


class PerceptionSpecialist(nn.Module):
    """
    L1: Visual perception specialist.
    Compresses pixels into a compact latent z.

    Architecture: Small CNN with aggressive downsampling to force compression.
    """

    def __init__(self, z_dim: int = 16, input_channels: int = 4,
                 input_h: int = 84, input_w: int = 84):
        super().__init__()
        self.z_dim = z_dim

        # Lightweight CNN: rapid spatial compression
        self.conv = nn.Sequential(
            nn.Conv2d(input_channels, 16, 5, stride=2, padding=2),  # 84->42
            nn.BatchNorm2d(16), nn.ReLU(),
            nn.Conv2d(16, 32, 3, stride=2, padding=1),              # 42->21
            nn.BatchNorm2d(32), nn.ReLU(),
            nn.Conv2d(32, 32, 3, stride=2, padding=1),              # 21->11
            nn.BatchNorm2d(32), nn.ReLU(),
            nn.AdaptiveAvgPool2d((3, 3)),                           # 11->3 (any input -> 3x3)
        )

        # Compute flattened dim after conv
        self.conv_out_dim = 32 * 3 * 3  # 288

        # Bottleneck: dense projection to z
        self.fc_mu = nn.Linear(self.conv_out_dim, z_dim)     # mean
        self.fc_logvar = nn.Linear(self.conv_out_dim, z_dim) # log variance (for VAE/KL)
        self.input_shape = (input_channels, input_h, input_w)

    def forward(self, x: torch.Tensor, sample: bool = True) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
        """
        Returns: (z, mu, logvar) where z is the compact representation
        If sample=False: z = mu (deterministic, for inference)
        """
        h = self.conv(x)                          # (B, 32, 3, 3)
        h = h.flatten(1)                          # (B, 288)
        mu = self.fc_mu(h)                        # (B, z_dim)
        logvar = self.fc_logvar(h)                # (B, z_dim)

        if sample and self.training:
            std = torch.exp(0.5 * logvar)
            eps = torch.randn_like(std)
            z = mu + eps * std                    # reparameterization trick
        else:
            z = mu                                 # deterministic

        return z, mu, logvar

class StrategicReasoner(nn.Module):
    """
    L2: Strategic reasoning from z.

    Receives compact visual representation z and optional action history.
    Outputs action token sequences.
    """

    def __init__(self, z_dim: int = 16, history_dim: int = 8,
                 d_model: int = 64, nhead: int = 2, n_layers: int = 1,
                 max_tokens: int = MAX_ACTION_TOKENS):
        super().__init__()
        self.z_dim = z_dim
        self.history_dim = history_dim

        # Input projection
        input_dim = z_dim + history_dim
        self.input_proj = nn.Linear(input_dim, d_model)

        # Small transformer decoder
        self.pos_embed = nn.Parameter(torch.randn(1, max_tokens, d_model) * 0.02)
        self.token_embed = nn.Embedding(ACTION_VOCAB_SIZE, d_model)

        decoder_layer = nn.TransformerDecoderLayer(
            d_model=d_model, nhead=nhead, dim_feedforward=d_model * 2,
            dropout=0.1, batch_first=True
        )
        self.transformer = nn.TransformerDecoder(decoder_layer, num_layers=n_layers)

        # Output head
        self.head = nn.Linear(d_model, ACTION_VOCAB_SIZE)
        self.max_tokens = max_tokens

        # Value head (for RL)
        self.value_head = nn.Sequential(
            nn.Linear(d_model, d_model),
            nn.ReLU(),
            nn.Linear(d_model, 1),
            nn.Tanh()
        )

    def forward(self, z: torch.Tensor,
                history: Optional[torch.Tensor] = None,
                target_tokens: Optional[torch.Tensor] = None
                ) -> Tuple[torch.Tensor, Optional[torch.Tensor]]:
        """
        z: (B, z_dim)
        history: (B, history_dim) — previous actions encoded
        target_tokens: (B, seq_len) for teacher forcing

        Returns: (action_logits, value)  where value is (B, 1)
        """
        B = z.shape[0]

        # Concatenate z + history (if available)
        if history is not None:
            h_input = torch.cat([z, history], dim=-1)
        else:
            # Zero-pad to match expected input dimension
            h_input = F.pad(z, (0, self.history_dim))  # pad with zeros to z_dim+hist_dim

        memory = self.input_proj(h_input).unsqueeze(1)   # (B, 1, d_model)

        # Get value
        value = self.value_head(memory.squeeze(1))       # (B, 1)

        if target_tokens is not None:
            # Teacher forcing
            tgt = self.token_embed(target_tokens)
            seq_len = target_tokens.shape[1]
            tgt = tgt + self.pos_embed[:, :seq_len, :]
            out = self.transformer(tgt, memory)
            logits = self.head(out)
        else:
            # Autoregressive
            bos = torch.zeros(B, 1, dtype=torch.long, device=z.device)
            tgt = self.token_embed(bos)
            tgt = tgt + self.pos_embed[:, :1, :]

            outputs = []
            for i in range(self.max_tokens):
                out = self.transformer(tgt, memory)
                logits = self.head(out[:, -1:, :])
                outputs.append(logits)
                next_token = logits.argmax(dim=-1)
                if (next_token == TOK_NOOP).all():
                    break
                next_emb = self.token_embed(next_token)
                next_emb = next_emb + self.pos_embed[:, i+1:i+2, :]
                tgt = torch.cat([tgt, next_emb], dim=1)

            logits = torch.cat(outputs, dim=1)

        return logits, value


class HierarchicalAgent(nn.Module):
    """
    Full hierarchical model: L1(z encoder) + L2(strategic reasoner).

    Trained end-to-end. After training, L1 can be extracted as a standalone
    perception specialist for the fast path.
    """

    def __init__(self, z_dim: int = 16, history_dim: int = 8):
        super().__init__()
        self.perception = PerceptionSpecialist(z_dim)
        self.reasoner = StrategicReasoner(z_dim, history_dim)
        self.z_dim = z_dim

    def forward(self, pixels: torch.Tensor,
                history: Optional[torch.Tensor] = None,
                target_tokens: Optional[torch.Tensor] = None,
                sample: bool = True
                ) -> Dict[str, torch.Tensor]:
        """
        Returns dict with:
          'action_logits': (B, seq_len, vocab)
          'value': (B, 1)
          'z': (B, z_dim)
          'z_mu': (B, z_dim)
          'z_logvar': (B, z_dim)
        """
        z, mu, logvar = self.perception(pixels, sample=sample)
        logits, value = self.reasoner(z, history, target_tokens)

        return {
            'action_logits': logits,
            'value': value,
            'z': z,
            'z_mu': mu,
            'z_logvar': logvar,
        }

    @torch.no_grad()
    def act(self, pixels: torch.Tensor,
            history: Optional[torch.Tensor] = None) -> torch.Tensor:
        """Fast inference: deterministic z -> greedy action tokens"""
        output = self.forward(pixels, history, sample=False)
        return output['action_logits'].argmax(dim=-1)

    @torch.no_grad()
    def encode(self, pixels: torch.Tensor) -> torch.Tensor:
        """Extract z vector from pixels (for analysis/visualization)"""
        z, _, _ = self.perception(pixels, sample=False)
        return z

    def get_num_params(self) -> Tuple[int, int, int]:
        """Returns (L1_params, L2_params, total_params)"""
        l1 = sum(p.numel() for p in self.perception.parameters())
        l2 = sum(p.numel() for p in self.reasoner.parameters())
        return l1, l2, l1 + l2

    def loss(self, output: Dict[str, torch.Tensor],
             target_tokens: torch.Tensor,
             target_value: Optional[torch.Tensor] = None,
             gamma: float = 0.001  # KL weight
             ) -> Dict[str, torch.Tensor]:
        """
        Combined training loss with information bottleneck.

        L_total = L_action + L_value + gamma * L_KL
        The KL term forces z to be compact and well-behaved (VAE-like prior).
        No reconstruction decoder needed — the downstream task loss ensures
        z retains sufficient information for decision-making.
        """
        logits = output['action_logits']
        B, seq_len, vocab = logits.shape

        # Action loss: cross-entropy
        L_action = F.cross_entropy(
            logits.reshape(-1, vocab),
            target_tokens.reshape(-1),
            ignore_index=TOK_NOOP
        )

        # Value loss (if target_value provided)
        L_value = torch.tensor(0.0, device=logits.device)
        if target_value is not None:
            L_value = F.mse_loss(output['value'], target_value)

        # KL divergence (VAE regularization — keeps z compact)
        mu = output['z_mu']
        logvar = output['z_logvar']
        L_kl = -0.5 * torch.mean(1 + logvar - mu.pow(2) - logvar.exp())

        total = L_action + L_value + gamma * L_kl

        return {
            'total': total,
            'action': L_action,
            'value': L_value,
            'kl': L_kl,
        }


def create_hierarchical_tictactoe() -> HierarchicalAgent:
    """Tiny hierarchical model for TicTacToe PoC"""
    return HierarchicalAgent(z_dim=16, history_dim=8)
