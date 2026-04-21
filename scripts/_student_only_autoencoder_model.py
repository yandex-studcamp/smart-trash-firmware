from __future__ import annotations

import torch
import torch.nn as nn
import torch.nn.functional as F


class ConvReLU(nn.Sequential):
    def __init__(
        self,
        in_channels: int,
        out_channels: int,
        *,
        kernel_size: int = 3,
        stride: int = 1,
    ) -> None:
        padding = kernel_size // 2
        super().__init__(
            nn.Conv2d(
                in_channels,
                out_channels,
                kernel_size=kernel_size,
                stride=stride,
                padding=padding,
                bias=True,
            ),
            nn.ReLU(inplace=True),
        )


class UpsampleConvBlock(nn.Module):
    def __init__(self, in_channels: int, out_channels: int) -> None:
        super().__init__()
        self.conv = ConvReLU(in_channels, out_channels, kernel_size=3, stride=1)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = F.interpolate(x, scale_factor=2, mode="nearest")
        return self.conv(x)


class StudentOnlyAutoencoder(nn.Module):
    def __init__(
        self,
        in_channels: int = 1,
        encoder_channels: tuple[int, int, int, int] = (16, 24, 32, 48),
        bottleneck_channels: int = 64,
    ) -> None:
        super().__init__()
        if in_channels != 1:
            raise ValueError("StudentOnlyAutoencoder expects a single grayscale input channel.")

        c1, c2, c3, c4 = encoder_channels
        self.encoder = nn.Sequential(
            ConvReLU(in_channels, c1, stride=1),
            ConvReLU(c1, c2, stride=2),
            ConvReLU(c2, c3, stride=2),
            ConvReLU(c3, c4, stride=2),
            ConvReLU(c4, bottleneck_channels, stride=2),
            ConvReLU(bottleneck_channels, bottleneck_channels, stride=1),
        )

        self.decoder = nn.Sequential(
            UpsampleConvBlock(bottleneck_channels, c4),
            UpsampleConvBlock(c4, c3),
            UpsampleConvBlock(c3, c2),
            UpsampleConvBlock(c2, c1),
            ConvReLU(c1, c1, stride=1),
            nn.Conv2d(c1, 1, kernel_size=3, stride=1, padding=1, bias=True),
            nn.Sigmoid(),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        latent = self.encoder(x)
        return self.decoder(latent)
