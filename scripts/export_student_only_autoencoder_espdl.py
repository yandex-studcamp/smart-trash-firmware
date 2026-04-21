from __future__ import annotations

import argparse
import sys
from pathlib import Path

from _model_export_common import (
    StudentAutoencoderExportConfig,
    build_calibration_loader,
    build_output_paths,
    export_model_to_onnx,
    export_onnx_to_espdl,
    load_torch_state_dict,
    resolve_default_weights_path,
    resolve_device,
    suggest_default_calibration_steps,
)
from _student_only_autoencoder_model import StudentOnlyAutoencoder


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT_DIR = PROJECT_ROOT / "models" / "exported"

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Export the StudentOnlyAutoencoder checkpoint to ONNX first and then "
            "quantize it to ESP-DL (.espdl/.info/.json)."
        ),
    )
    parser.add_argument(
        "--model_path",
        type=Path,
        required=True,
        help=(
            "Compatibility input. You can pass the original architecture .py file, "
            "the experiment directory, the config.json, or the checkpoint .pth file."
        ),
    )
    parser.add_argument(
        "--experiment_dir",
        type=Path,
        default=None,
        help="Experiment directory with config.json and weights/.",
    )
    parser.add_argument(
        "--config_path",
        type=Path,
        default=None,
        help="Path to experiment config.json. Overrides auto-detection.",
    )
    parser.add_argument(
        "--weights_path",
        type=Path,
        default=None,
        help="Path to the .pth checkpoint. Overrides auto-detection.",
    )
    parser.add_argument(
        "--output_path_name",
        type=str,
        required=True,
        help="Base output file name without extension.",
    )
    parser.add_argument(
        "--output_dir",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
        help="Directory where .onnx/.espdl/.info/.json will be written.",
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
        help="Execution device for ONNX quantization, usually `cpu` or `cuda`.",
    )
    parser.add_argument(
        "--calibration_samples",
        type=int,
        default=64,
        help="Maximum number of normal train samples to use for calibration.",
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


def resolve_paths(args: argparse.Namespace) -> tuple[Path | None, Path | None, Path, Path]:
    model_path = args.model_path.expanduser().resolve()
    if not model_path.exists():
        raise FileNotFoundError(f"--model_path was not found: {model_path}")

    model_definition_path: Path | None = None
    experiment_dir = args.experiment_dir.expanduser().resolve() if args.experiment_dir else None
    config_path = args.config_path.expanduser().resolve() if args.config_path else None
    weights_path = args.weights_path.expanduser().resolve() if args.weights_path else None

    if model_path.is_dir():
        experiment_dir = model_path if experiment_dir is None else experiment_dir
    elif model_path.suffix.lower() == ".py":
        model_definition_path = model_path
    elif model_path.suffix.lower() == ".json":
        config_path = model_path if config_path is None else config_path
    elif model_path.suffix.lower() in {".pth", ".pt"}:
        weights_path = model_path if weights_path is None else weights_path
    else:
        raise ValueError(
            "--model_path must point to a .py architecture file, experiment directory, config.json, or .pth/.pt checkpoint.",
        )

    if experiment_dir is None:
        if config_path is not None:
            experiment_dir = config_path.parent
        elif weights_path is not None and weights_path.parent.name == "weights":
            experiment_dir = weights_path.parent.parent

    if experiment_dir is not None:
        if config_path is None:
            config_path = experiment_dir / "config.json"
        if weights_path is None:
            weights_path = resolve_default_weights_path(experiment_dir)

    if config_path is None or not config_path.exists():
        raise FileNotFoundError("Could not resolve config.json. Pass --experiment_dir or --config_path explicitly.")
    if weights_path is None or not weights_path.exists():
        raise FileNotFoundError("Could not resolve checkpoint .pth file. Pass --experiment_dir or --weights_path explicitly.")

    return model_definition_path, experiment_dir, config_path, weights_path


def main() -> None:
    args = parse_args()
    device = resolve_device(args.device)
    model_definition_path, experiment_dir, config_path, weights_path = resolve_paths(args)

    config = StudentAutoencoderExportConfig.from_json(config_path)
    calibration_loader, example_input, calibration_sample_count = build_calibration_loader(
        config,
        batch_size=args.calibration_batch_size,
        max_samples=args.calibration_samples,
    )
    calibration_steps = args.calibration_steps
    if calibration_steps is None:
        calibration_steps = suggest_default_calibration_steps(
            num_samples=calibration_sample_count,
            batch_size=args.calibration_batch_size,
        )

    model = StudentOnlyAutoencoder(
        in_channels=config.input_channels,
        encoder_channels=config.encoder_channels,
        bottleneck_channels=config.bottleneck_channels,
    )
    state_dict = load_torch_state_dict(weights_path)
    model.load_state_dict(state_dict, strict=True)
    model.eval()

    output_dir = args.output_dir.expanduser().resolve()
    onnx_path, espdl_path = build_output_paths(output_dir, args.output_path_name)

    export_model_to_onnx(
        model=model,
        example_input=example_input,
        output_path=onnx_path,
        opset_version=args.onnx_opset,
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

    print("=== StudentOnlyAutoencoder export finished ===")
    if model_definition_path is not None:
        print(f"Model definition reference: {model_definition_path}")
    if experiment_dir is not None:
        print(f"Experiment dir: {experiment_dir}")
    print(f"Config: {config_path}")
    print(f"Weights: {weights_path}")
    print(f"Input shape: {tuple(example_input.shape)}")
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
