#!/usr/bin/env python3
"""
Receive latest JPEG photos from ESP32-CAM camera-capture boot profile.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Poll ESP32-CAM /photo.jpg endpoint and save received JPEG photos.",
    )
    parser.add_argument("--host", default="192.168.4.1", help="ESP32-CAM host/IP (default: 192.168.4.1)")
    parser.add_argument("--port", type=int, default=80, help="HTTP port (default: 80)")
    parser.add_argument("--path", default="/photo.jpg", help="Photo endpoint path (default: /photo.jpg)")
    parser.add_argument("--interval", type=float, default=0.5, help="Polling interval in seconds (default: 0.5)")
    parser.add_argument("--timeout", type=float, default=5.0, help="HTTP timeout in seconds (default: 5.0)")
    parser.add_argument("--out", default="logs/wifi-photos", help="Output folder (default: logs/wifi-photos)")
    parser.add_argument("--max", type=int, default=0, help="Max number of saved photos, 0 = unlimited")
    return parser.parse_args()


def parse_seq(value: str | None) -> int | None:
    if value is None:
        return None
    try:
        return int(value.strip())
    except (TypeError, ValueError):
        return None


def build_url(host: str, port: int, path: str) -> str:
    clean_path = path if path.startswith("/") else f"/{path}"
    return f"http://{host}:{port}{clean_path}"


def fetch_photo(url: str, timeout_sec: float) -> tuple[bytes, int | None, str]:
    request = urllib.request.Request(
        url,
        headers={
            "Cache-Control": "no-cache",
            "Pragma": "no-cache",
        },
    )
    with urllib.request.urlopen(request, timeout=timeout_sec) as response:
        status = getattr(response, "status", 200)
        if status != 200:
            raise RuntimeError(f"HTTP status {status}")
        payload = response.read()
        seq = parse_seq(response.headers.get("X-Photo-Seq"))
        content_type = response.headers.get("Content-Type", "")
        return payload, seq, content_type


def main() -> int:
    args = parse_args()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    url = build_url(args.host, args.port, args.path)
    print(f"[receiver] Source: {url}")
    print(f"[receiver] Save dir: {out_dir.resolve()}")
    print("[receiver] Press Ctrl+C to stop")

    saved = 0
    last_seq: int | None = None
    last_digest: str | None = None

    try:
        while True:
            try:
                payload, seq, content_type = fetch_photo(url, args.timeout)
                if not payload:
                    time.sleep(args.interval)
                    continue
                if "image/jpeg" not in content_type.lower():
                    print(f"[receiver] warning: unexpected content-type '{content_type}'")

                digest = hashlib.sha1(payload).hexdigest()
                duplicate = False
                if seq is not None and last_seq is not None and seq == last_seq:
                    duplicate = True
                if seq is None and last_digest is not None and digest == last_digest:
                    duplicate = True

                if duplicate:
                    time.sleep(args.interval)
                    continue

                saved += 1
                timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
                seq_part = f"seq{seq:06d}_" if seq is not None else ""
                filename = f"photo_{saved:05d}_{seq_part}{timestamp}.jpg"
                target = out_dir / filename
                target.write_bytes(payload)

                print(f"[receiver] saved {target} ({len(payload)} bytes)")

                last_seq = seq
                last_digest = digest

                if args.max > 0 and saved >= args.max:
                    print(f"[receiver] reached --max={args.max}")
                    break

                time.sleep(args.interval)

            except urllib.error.HTTPError as err:
                body = ""
                try:
                    body = err.read().decode("utf-8", errors="ignore").strip()
                except Exception:
                    body = ""
                if body:
                    print(f"[receiver] HTTP {err.code}: {body}")
                else:
                    print(f"[receiver] HTTP {err.code}")
                time.sleep(args.interval)
            except urllib.error.URLError as err:
                print(f"[receiver] network error: {err}")
                time.sleep(args.interval)
            except RuntimeError as err:
                print(f"[receiver] {err}")
                time.sleep(args.interval)
            except TimeoutError:
                print("[receiver] timeout")
                time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n[receiver] stopped by user")

    print(f"[receiver] total saved: {saved}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
