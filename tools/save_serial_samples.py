#!/usr/bin/env python3
"""Extract image debug samples from ESP-IDF monitor logs.

The firmware prints samples as:

    SAMPLE_BEGIN id=1 format=jpeg width=64 height=48 ... bytes=5312
    SAMPLE_DATA 50360a...
    SAMPLE_END id=1

This tool accepts either a saved monitor log file or stdin. It intentionally
handles UTF-16 logs because Windows PowerShell `Tee-Object` can create them.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Iterable


BEGIN_RE = re.compile(
    r"SAMPLE_BEGIN\s+id=(?P<id>\d+)\s+format=(?P<format>\S+)\s+"
    r"width=(?P<width>\d+)\s+height=(?P<height>\d+).*?\s+bytes=(?P<bytes>\d+)"
)
DATA_RE = re.compile(r"SAMPLE_DATA\s+(?P<hex>[0-9a-fA-F]+)")
END_RE = re.compile(r"SAMPLE_END\s+id=(?P<id>\d+)")
KIND_RE = re.compile(r"\bkind=(?P<kind>[A-Za-z0-9_-]+)")
INFERENCE_ID_RE = re.compile(r"\binference_id=(?P<inference_id>\d+)")


def decode_log_bytes(data: bytes) -> str:
    """Decode monitor log bytes from common Windows/UTF encodings."""
    if data.startswith(b"\xff\xfe"):
        return data.decode("utf-16-le", errors="replace")
    if data.startswith(b"\xfe\xff"):
        return data.decode("utf-16-be", errors="replace")
    if data.startswith(b"\xef\xbb\xbf"):
        return data.decode("utf-8-sig", errors="replace")

    sample = data[:4096]
    if sample:
        even_nuls = sample[0::2].count(0)
        odd_nuls = sample[1::2].count(0)
        # PowerShell 5 often writes UTF-16LE without a BOM in redirected logs.
        if odd_nuls > max(16, len(sample) // 8):
            return data.decode("utf-16-le", errors="replace")
        if even_nuls > max(16, len(sample) // 8):
            return data.decode("utf-16-be", errors="replace")

    return data.decode("utf-8", errors="replace")


def read_log_lines(path: Path) -> list[str]:
    return decode_log_bytes(path.read_bytes()).splitlines()


def sample_path(out_dir: Path, sample_id: int, suffix: str, kind: str | None, inference_id: int | None) -> Path:
    safe_kind = re.sub(r"[^A-Za-z0-9_-]+", "_", (kind or "").strip()).strip("_")
    if inference_id is not None and safe_kind:
        return out_dir / f"infer_{inference_id:06d}_{safe_kind}_{sample_id:04d}.{suffix}"
    if safe_kind:
        return out_dir / f"sample_{sample_id:04d}_{safe_kind}.{suffix}"
    return out_dir / f"sample_{sample_id:04d}.{suffix}"


def next_sample_index(out_dir: Path) -> int:
    max_index = 0
    for path in out_dir.glob("*.*"):
        stem = path.stem
        parts = stem.split("_")
        if not parts:
            continue
        last = parts[-1]
        if last.isdigit():
            max_index = max(max_index, int(last))
    return max_index + 1


def sample_suffix(sample_format: str) -> str:
    fmt = sample_format.strip().lower()
    if fmt in ("jpg", "jpeg"):
        return "jpg"
    if fmt in ("ppm", "png", "bmp"):
        return fmt
    if fmt and all(ch.isalnum() for ch in fmt):
        return fmt
    return "bin"


def save_sample(
    out_dir: Path,
    sample_index: int,
    suffix: str,
    payload: bytes,
    meta_line: str,
    kind: str | None,
    inference_id: int | None,
) -> None:
    ppm_path = sample_path(out_dir, sample_index, suffix, kind, inference_id)
    meta_path = sample_path(out_dir, sample_index, "log", kind, inference_id)
    ppm_path.write_bytes(payload)
    meta_path.write_text(meta_line.rstrip() + "\n", encoding="utf-8")
    print(f"[sample-save] {ppm_path} ({len(payload)} bytes)", file=sys.stderr, flush=True)


def process_lines(lines: Iterable[str], out_dir: Path, mirror: bool) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)

    current_id: int | None = None
    current_format = "bin"
    current_kind: str | None = None
    current_inference_id: int | None = None
    expected_bytes = 0
    meta_line = ""
    chunks: list[str] = []
    saved = 0
    sample_index = next_sample_index(out_dir)

    for raw_line in lines:
        line = raw_line.rstrip("\r\n")
        if mirror:
            print(line, flush=True)

        begin = BEGIN_RE.search(line)
        if begin:
            current_id = int(begin.group("id"))
            current_format = begin.group("format")
            expected_bytes = int(begin.group("bytes"))
            kind_match = KIND_RE.search(line)
            current_kind = kind_match.group("kind") if kind_match else None
            inference_match = INFERENCE_ID_RE.search(line)
            current_inference_id = int(inference_match.group("inference_id")) if inference_match else None
            meta_line = line
            chunks = []
            continue

        if current_id is None:
            continue

        data = DATA_RE.search(line)
        if data:
            chunks.append(data.group("hex"))
            continue

        end = END_RE.search(line)
        if end and int(end.group("id")) == current_id:
            try:
                payload = bytes.fromhex("".join(chunks))
            except ValueError as exc:
                print(
                    f"[sample-save] sample {current_id}: invalid hex payload: {exc}",
                    file=sys.stderr,
                    flush=True,
                )
            else:
                if len(payload) != expected_bytes:
                    print(
                        f"[sample-save] sample {current_id}: byte count mismatch "
                        f"expected={expected_bytes} actual={len(payload)}",
                        file=sys.stderr,
                        flush=True,
                    )
                save_sample(
                    out_dir,
                    sample_index,
                    sample_suffix(current_format),
                    payload,
                    meta_line,
                    current_kind,
                    current_inference_id,
                )
                saved += 1
                sample_index += 1

            current_id = None
            current_format = "bin"
            current_kind = None
            current_inference_id = None
            expected_bytes = 0
            meta_line = ""
            chunks = []

    if current_id is not None:
        print(f"[sample-save] sample {current_id}: incomplete sample block", file=sys.stderr, flush=True)

    return saved


def main() -> int:
    parser = argparse.ArgumentParser(description="Save firmware SAMPLE_DATA blocks as image files.")
    parser.add_argument("input", nargs="?", help="Monitor log file. If omitted, reads stdin.")
    parser.add_argument("--out", default="logs/samples", help="Output directory.")
    parser.add_argument(
        "--mirror",
        action="store_true",
        help="When reading stdin, also print monitor lines back to stdout.",
    )
    args = parser.parse_args()

    out_dir = Path(args.out)
    if args.input:
        saved = process_lines(read_log_lines(Path(args.input)), out_dir, mirror=False)
    else:
        saved = process_lines(sys.stdin, out_dir, mirror=args.mirror)

    print(f"[sample-save] saved {saved} sample(s)", file=sys.stderr, flush=True)
    return 0 if saved > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
