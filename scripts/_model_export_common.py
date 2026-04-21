from __future__ import annotations

import csv
import json
import math
from dataclasses import dataclass
from numbers import Number
from pathlib import Path
from typing import Any

import numpy as np
import torch
from PIL import Image
from torch.utils.data import DataLoader, Dataset, Subset


REQUIREMENTS_HINT = (
    "Install the host-side export dependencies first: "
    "`python -m pip install -r scripts/requirements_model_export.txt`."
)


@dataclass(frozen=True, slots=True)
class ROIConfig:
    x: int
    y: int
    w: int
    h: int

    @classmethod
    def from_json(cls, payload: dict[str, Any] | None) -> "ROIConfig | None":
        if payload is None:
            return None
        return cls(
            x=int(payload["x"]),
            y=int(payload["y"]),
            w=int(payload["w"]),
            h=int(payload["h"]),
        )

    def as_xywh(self) -> tuple[int, int, int, int]:
        return self.x, self.y, self.w, self.h

    def validate(self) -> None:
        if self.w <= 0 or self.h <= 0:
            raise ValueError("ROI width and height must be positive.")
        if self.x < 0 or self.y < 0:
            raise ValueError("ROI x and y must be non-negative.")


@dataclass(frozen=True, slots=True)
class StudentAutoencoderExportConfig:
    train_csv: str
    valid_csv: str
    test_csv: str
    img_dir: str
    input_size: int
    input_channels: int
    roi: ROIConfig | None
    encoder_channels: tuple[int, int, int, int]
    bottleneck_channels: int

    @classmethod
    def from_json(cls, config_path: Path) -> "StudentAutoencoderExportConfig":
        payload = json.loads(config_path.read_text(encoding="utf-8"))
        config = cls(
            train_csv=str(payload["train_csv"]),
            valid_csv=str(payload["valid_csv"]),
            test_csv=str(payload["test_csv"]),
            img_dir=str(payload["img_dir"]),
            input_size=int(payload["input_size"]),
            input_channels=int(payload["input_channels"]),
            roi=ROIConfig.from_json(payload.get("roi")),
            encoder_channels=tuple(int(value) for value in payload["encoder_channels"]),
            bottleneck_channels=int(payload["bottleneck_channels"]),
        )
        if len(config.encoder_channels) != 4:
            raise ValueError("encoder_channels must contain exactly 4 values.")
        if config.input_channels != 1:
            raise ValueError("This exporter currently supports only 1-channel grayscale inputs.")
        if config.roi is not None:
            config.roi.validate()
        return config


class StudentAutoencoderCalibrationDataset(Dataset[torch.Tensor]):
    def __init__(
        self,
        csv_file: Path,
        root_dir: Path,
        image_size: int,
        roi: ROIConfig | None = None,
        normal_only: bool = True,
    ) -> None:
        self.root_dir = root_dir
        self.image_size = image_size
        self.roi = roi
        self.rows = self._read_rows(csv_file, normal_only=normal_only)
        if not self.rows:
            raise ValueError(f"Calibration dataset is empty after filtering: {csv_file}")

    @staticmethod
    def _read_rows(csv_file: Path, *, normal_only: bool) -> list[dict[str, str]]:
        with csv_file.open("r", encoding="utf-8", newline="") as file:
            reader = csv.DictReader(file)
            if reader.fieldnames is None or "file_path" not in reader.fieldnames:
                raise ValueError(f"`file_path` column is required in {csv_file}.")

            rows: list[dict[str, str]] = []
            for row in reader:
                label = resolve_anomaly_label(row)
                if normal_only and label != 0:
                    continue
                rows.append(row)
            return rows

    def __len__(self) -> int:
        return len(self.rows)

    def __getitem__(self, index: int) -> torch.Tensor:
        row = self.rows[index]
        image_path = resolve_image_path(row["file_path"], self.root_dir)

        image = Image.open(image_path).convert("L")
        if self.roi is not None:
            x, y, width, height = self.roi.as_xywh()
            image = image.crop((x, y, x + width, y + height))

        image = image.resize((self.image_size, self.image_size), Image.Resampling.BILINEAR)
        return grayscale_pil_to_tensor(image)


def resolve_anomaly_label(row: dict[str, Any]) -> int:
    for column in ("label_id", "label", "target", "class_name", "class", "is_anomaly"):
        if column not in row:
            continue

        value = row[column]
        if value is None:
            continue
        if isinstance(value, Number):
            return int(value != 0)

        normalized = str(value).strip().lower()
        if not normalized:
            continue
        if normalized in {"normal", "ok", "good", "0"}:
            return 0
        if normalized in {"anomaly", "anomalous", "defect", "1"}:
            return 1
        try:
            return int(float(normalized) != 0.0)
        except ValueError:
            continue

    path_parts = Path(str(row["file_path"]).replace("\\", "/")).parts
    return 1 if "anomaly" in {part.lower() for part in path_parts} else 0


def resolve_image_path(file_path: str, root_dir: Path) -> Path:
    path = Path(file_path)
    if path.is_absolute():
        return path
    return root_dir / path


def grayscale_pil_to_tensor(image: Image.Image) -> torch.Tensor:
    array = np.asarray(image, dtype=np.float32) / 255.0
    return torch.from_numpy(array).unsqueeze(0)


def build_calibration_loader(
    config: StudentAutoencoderExportConfig,
    *,
    batch_size: int,
    max_samples: int | None,
) -> tuple[DataLoader[torch.Tensor], torch.Tensor, int]:
    dataset: Dataset[torch.Tensor] = StudentAutoencoderCalibrationDataset(
        csv_file=Path(config.train_csv),
        root_dir=Path(config.img_dir),
        image_size=config.input_size,
        roi=config.roi,
        normal_only=True,
    )

    if max_samples is not None and max_samples > 0 and len(dataset) > max_samples:
        dataset = Subset(dataset, range(max_samples))

    loader = DataLoader(
        dataset=dataset,
        batch_size=batch_size,
        shuffle=False,
        drop_last=False,
        collate_fn=lambda batch: torch.stack(batch, dim=0),
    )
    example_input = dataset[0].unsqueeze(0)
    return loader, example_input, len(dataset)


def resolve_default_weights_path(experiment_dir: Path) -> Path:
    best_model_path = experiment_dir / "weights" / "best_model.pth"
    if best_model_path.exists():
        return best_model_path

    candidates = sorted(
        (experiment_dir / "weights").glob("latest_model_epoch_*.pth"),
        reverse=True,
    )
    if candidates:
        return candidates[0]

    raise FileNotFoundError(
        "Could not find `best_model.pth` or any `latest_model_epoch_*.pth` "
        f"inside {experiment_dir / 'weights'}.",
    )


def export_model_to_onnx(
    model: torch.nn.Module,
    example_input: torch.Tensor,
    output_path: Path,
    *,
    opset_version: int,
) -> None:
    try:
        import onnx
    except ModuleNotFoundError as exc:
        raise RuntimeError(
            "Missing Python package `onnx`. "
            + REQUIREMENTS_HINT,
        ) from exc

    output_path.parent.mkdir(parents=True, exist_ok=True)
    model = model.eval().cpu()
    example_input = example_input.detach().cpu()

    with torch.inference_mode():
        torch.onnx.export(
            model,
            example_input,
            output_path,
            export_params=True,
            external_data=False,
            do_constant_folding=True,
            input_names=["input"],
            output_names=["reconstruction"],
            opset_version=opset_version,
        )

    onnx_model = onnx.load(output_path)
    onnx.checker.check_model(onnx_model)


def export_torchscript_to_onnx(
    model: torch.jit.RecursiveScriptModule,
    example_input: torch.Tensor,
    output_path: Path,
    *,
    opset_version: int,
    output_names: list[str],
) -> None:
    try:
        import onnx
    except ModuleNotFoundError as exc:
        raise RuntimeError(
            "Missing Python package `onnx`. "
            + REQUIREMENTS_HINT,
        ) from exc

    output_path.parent.mkdir(parents=True, exist_ok=True)
    model = model.eval().cpu()
    example_input = example_input.detach().cpu()

    with torch.inference_mode():
        # The new dynamo-based exporter does not accept ScriptModule input.
        torch.onnx.export(
            model,
            example_input,
            output_path,
            export_params=True,
            external_data=False,
            do_constant_folding=True,
            input_names=["input"],
            output_names=output_names,
            opset_version=opset_version,
            dynamo=False,
        )

    onnx_model = onnx.load(output_path)
    onnx.checker.check_model(onnx_model)


def export_onnx_to_espdl(
    onnx_path: Path,
    espdl_path: Path,
    calibration_loader: DataLoader[torch.Tensor],
    *,
    example_input: torch.Tensor,
    calibration_steps: int | None,
    target: str,
    num_of_bits: int,
    device: str,
    export_test_values: bool,
    verbose: int,
    error_report: bool,
) -> tuple[Path, Path, Path, int]:
    try:
        from esp_ppq.api import QuantizationSettingFactory, espdl_quantize_onnx
    except ModuleNotFoundError as exc:
        raise RuntimeError(
            "Missing Python package `esp-ppq`. "
            + REQUIREMENTS_HINT,
        ) from exc

    espdl_path.parent.mkdir(parents=True, exist_ok=True)
    quant_setting = QuantizationSettingFactory.espdl_setting()
    max_available_steps = max(len(calibration_loader), 1)
    effective_steps = max_available_steps if calibration_steps is None else max(1, min(calibration_steps, max_available_steps))

    def collate_fn(batch: torch.Tensor) -> torch.Tensor:
        return batch.to(device=device, dtype=torch.float32)

    espdl_quantize_onnx(
        onnx_import_file=str(onnx_path),
        espdl_export_file=str(espdl_path),
        calib_dataloader=calibration_loader,
        calib_steps=effective_steps,
        input_shape=list(example_input.shape),
        inputs=[example_input.to(device=device, dtype=torch.float32)],
        target=target,
        num_of_bits=num_of_bits,
        collate_fn=collate_fn,
        setting=quant_setting,
        device=device,
        error_report=error_report,
        skip_export=False,
        export_test_values=export_test_values,
        verbose=verbose,
    )

    info_path = espdl_path.with_suffix(".info")
    json_path = espdl_path.with_suffix(".json")
    return espdl_path, info_path, json_path, effective_steps


def load_torch_state_dict(weights_path: Path) -> dict[str, torch.Tensor]:
    state_dict = torch.load(weights_path, map_location="cpu")
    if not isinstance(state_dict, dict):
        raise TypeError(f"Unsupported checkpoint format in {weights_path}. Expected a state_dict dictionary.")
    return state_dict


def resolve_device(device_name: str) -> str:
    normalized = device_name.strip().lower()
    if normalized == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA was requested, but torch.cuda.is_available() is False.")
    return normalized


def build_output_paths(output_dir: Path, output_name: str) -> tuple[Path, Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    return output_dir / f"{output_name}.onnx", output_dir / f"{output_name}.espdl"


def build_output_paths_from_espdl_path(espdl_path: Path) -> tuple[Path, Path]:
    espdl_path.parent.mkdir(parents=True, exist_ok=True)
    if espdl_path.suffix.lower() != ".espdl":
        raise ValueError("--output_path must have the `.espdl` extension.")
    return espdl_path.with_suffix(".onnx"), espdl_path


def suggest_default_calibration_steps(num_samples: int, batch_size: int) -> int:
    return max(1, math.ceil(num_samples / max(batch_size, 1)))
