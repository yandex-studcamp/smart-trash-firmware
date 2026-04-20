from pathlib import Path
import os
import platform
import shutil
import subprocess
import sys


def load_env_value(env_file: str, key: str) -> str | None:
    path = Path(env_file)
    if not path.exists():
        return None

    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue

        k, v = line.split("=", 1)
        if k.strip() == key:
            return v.strip().strip('"').strip("'")

    return None


def main():
    activate_path = load_env_value(".env", "ESP_IDF_VENV_PATH")
    if not activate_path:
        print("ESP_IDF_VENV_PATH was not found in .env", file=sys.stderr)
        sys.exit(1)

    activate_path = os.path.expandvars(os.path.expanduser(activate_path))
    activate_file = Path(activate_path)

    if not activate_file.exists():
        print(f"File not found: {activate_file}", file=sys.stderr)
        sys.exit(1)

    suffix = activate_file.suffix.lower()
    system = platform.system().lower()

    if suffix == ".ps1":
        shell = shutil.which("pwsh") or shutil.which("powershell")
        if not shell:
            print("pwsh or powershell was not found", file=sys.stderr)
            sys.exit(1)

        subprocess.run([
            shell,
            "-NoExit",
            "-ExecutionPolicy", "Bypass",
            "-Command", f'& "{activate_file}"'
        ])
        return

    if suffix in {".bat", ".cmd"}:
        if system != "windows":
            print("BAT/CMD activation scripts can only run on Windows", file=sys.stderr)
            sys.exit(1)

        subprocess.run([
            "cmd.exe",
            "/K",
            str(activate_file)
        ])
        return

    shell = os.environ.get("SHELL") or shutil.which("bash") or shutil.which("zsh") or shutil.which("sh")
    if not shell:
        print("No shell was found for running the activation script", file=sys.stderr)
        sys.exit(1)

    subprocess.run([
        shell,
        "-i",
        "-c",
        f'source "{activate_file}" && exec "{shell}" -i'
    ])


if __name__ == "__main__":
    main()
    