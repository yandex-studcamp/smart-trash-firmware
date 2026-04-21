from __future__ import annotations

import argparse
import sys
from pathlib import Path

import torch

from _model_export_common import (
    StudentAutoencoderExportConfig,
    build_calibration_loader,
    build_output_paths_from_espdl_path,
    export_onnx_to_espdl,
    export_torchscript_to_onnx,
    resolve_device,
    suggest_default_calibration_steps,
)


if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Export a score-only StudentAutoencoder TorchScript wrapper (.pt) "
            "to ONNX and ESP-DL."
        ),
    )
    parser.add_argument(
        "--model_path",
        type=Path,
        required=True,
        help="Path to the TorchScript wrapper model (.pt).",
    )
    parser.add_argument(
        "--config_path",
        type=Path,
        default=None,
        help="Optional path to experiment config.json. If omitted, it is inferred from model_path.",
    )
    parser.add_argument(
        "--output_path",
        type=Path,
        required=True,
        help="Full target path for the .espdl file.",
    )
    parser.add_argument(
        "--target",
        type=str,
        default="c",
        choices=["c", "esp32s3", "esp32p4"],
        help="ESP-PPQ target. Use `c` for classic ESP32 firmware builds.",
    )
    parser.add_argument(
        "--bits",
        type=int,
        default=8,
        choices=[8, 16],
        help="Quantization bit width for ESP-DL.",
    )
    parser.add_argument(
        "--device",
        type=str,
        default="cpu",
        help="Execution device for ONNX export and quantization, usually `cpu`.",
    )
    parser.add_argument(
        "--calibration_source",
        type=str,
        default="train",
        choices=["train", "validation"],
        help="Which split to use for quantization calibration.",
    )
    parser.add_argument(
        "--calibration_samples",
        type=int,
        default=64,
        help="Maximum number of normal samples to use for quantization calibration.",
    )
    parser.add_argument(
        "--calibration_batch_size",
        type=int,
        default=8,
        help="Calibration dataloader batch size.",
    )
    parser.add_argument(
        "--calibration_steps",
        type=int,
        default=None,
        help="Number of calibration steps. Defaults to enough steps to cover the chosen samples.",
    )
    parser.add_argument(
        "--onnx_opset",
        type=int,
        default=18,
        help="ONNX opset version to export.",
    )
    parser.add_argument(
        "--disable_test_values",
        action="store_true",
        help="Disable embedding test input/output values into the exported .espdl file.",
    )
    parser.add_argument(
        "--error_report",
        action="store_true",
        help="Enable ESP-PPQ error analysis. Slower, but useful for debugging quantization.",
    )
    parser.add_argument(
        "--verbose",
        type=int,
        default=0,
        help="ESP-PPQ verbosity level.",
    )
    return parser.parse_args()


def resolve_config_path(model_path: Path, explicit_config_path: Path | None) -> Path:
    if explicit_config_path is not None:
        config_path = explicit_config_path.expanduser().resolve()
        if not config_path.exists():
            raise FileNotFoundError(f"Config file was not found: {config_path}")
        return config_path

    model_path = model_path.expanduser().resolve()
    if model_path.parent.name == "artifacts":
        candidate = model_path.parent.parent / "config.json"
        if candidate.exists():
            return candidate

    raise FileNotFoundError(
        "Could not infer config.json from model_path. Pass --config_path explicitly.",
    )


def make_calibration_config(
    config: StudentAutoencoderExportConfig,
    *,
    calibration_source: str,
) -> StudentAutoencoderExportConfig:
    calibration_csv = config.train_csv if calibration_source == "train" else config.valid_csv
    return StudentAutoencoderExportConfig(
        train_csv=calibration_csv,
        valid_csv=config.valid_csv,
        test_csv=config.test_csv,
        img_dir=config.img_dir,
        input_size=config.input_size,
        input_channels=config.input_channels,
        roi=config.roi,
        encoder_channels=config.encoder_channels,
        bottleneck_channels=config.bottleneck_channels,
    )


def main() -> None:
    args = parse_args()
    device = resolve_device(args.device)

    model_path = args.model_path.expanduser().resolve()
    if not model_path.exists():
        raise FileNotFoundError(f"Model file was not found: {model_path}")

    config_path = resolve_config_path(model_path, args.config_path)
    config = StudentAutoencoderExportConfig.from_json(config_path)
    calibration_config = make_calibration_config(
        config,
        calibration_source=args.calibration_source,
    )

    calibration_loader, example_input, calibration_sample_count = build_calibration_loader(
        calibration_config,
        batch_size=args.calibration_batch_size,
        max_samples=args.calibration_samples,
    )
    calibration_steps = args.calibration_steps
    if calibration_steps is None:
        calibration_steps = suggest_default_calibration_steps(
            num_samples=calibration_sample_count,
            batch_size=args.calibration_batch_size,
        )

    model = torch.jit.load(model_path, map_location=device)
    model.eval()

    output_path = args.output_path.expanduser().resolve()
    onnx_path, espdl_path = build_output_paths_from_espdl_path(output_path)

    export_torchscript_to_onnx(
        model=model,
        example_input=example_input,
        output_path=onnx_path,
        opset_version=args.onnx_opset,
        output_names=["score"],
    )
    espdl_path, info_path, json_path, effective_steps = export_onnx_to_espdl(
        onnx_path=onnx_path,
        espdl_path=espdl_path,
        calibration_loader=calibration_loader,
        example_input=example_input,
        calibration_steps=calibration_steps,
        target=args.target,
        num_of_bits=args.bits,
        device=device,
        export_test_values=not args.disable_test_values,
        verbose=args.verbose,
        error_report=args.error_report,
    )

    print("=== StudentAutoencoder score wrapper export finished ===")
    print(f"Model: {model_path}")
    print(f"Config: {config_path}")
    print(f"Calibration source: {args.calibration_source}")
    print(f"Calibration samples: {calibration_sample_count}")
    print(f"Calibration steps: {effective_steps}")
    print(f"Target: {args.target}")
    print(f"Bits: {args.bits}")
    print(f"ONNX: {onnx_path}")
    print(f"ESPDL: {espdl_path}")
    print(f"INFO: {info_path}")
    print(f"JSON: {json_path}")


if __name__ == "__main__":
    main()
