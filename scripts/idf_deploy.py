from pathlib import Path
import os
import platform
import shutil
import subprocess
import sys


def load_env_file(env_file: str) -> dict[str, str]:
    path = Path(env_file)
    if not path.exists():
        return {}

    data: dict[str, str] = {}

    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue

        key, value = line.split("=", 1)
        data[key.strip()] = value.strip().strip('"').strip("'")

    return data


def get_current_target(project_dir: Path) -> str | None:
    sdkconfig = project_dir / "sdkconfig"
    if not sdkconfig.exists():
        return None

    for line in sdkconfig.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = line.strip()
        if line.startswith("CONFIG_IDF_TARGET="):
            return line.split("=", 1)[1].strip().strip('"')

    return None


def run_checked(command: list[str]) -> None:
    result = subprocess.run(command)
    if result.returncode != 0:
        sys.exit(result.returncode)


def main() -> None:
    env = load_env_file(".env")

    activate_path = env.get("ESP_IDF_VENV_PATH")
    project_path = env.get("ESP_IDF_PROJECT_PATH")
    target = env.get("ESP_IDF_TARGET")
    port = env.get("ESP_IDF_PORT")
    baud = env.get("ESP_IDF_BAUD", "115200")

    if not activate_path:
        print("ESP_IDF_VENV_PATH was not found in .env", file=sys.stderr)
        sys.exit(1)

    if not project_path:
        print("ESP_IDF_PROJECT_PATH was not found in .env", file=sys.stderr)
        sys.exit(1)

    if not target:
        print("ESP_IDF_TARGET was not found in .env", file=sys.stderr)
        sys.exit(1)

    if not port:
        print("ESP_IDF_PORT was not found in .env", file=sys.stderr)
        sys.exit(1)

    activate_file = Path(os.path.expandvars(os.path.expanduser(activate_path)))
    project_dir = Path(os.path.expandvars(os.path.expanduser(project_path)))

    if not activate_file.exists():
        print(f"Activation script was not found: {activate_file}", file=sys.stderr)
        sys.exit(1)

    if not project_dir.exists():
        print(f"Project directory was not found: {project_dir}", file=sys.stderr)
        sys.exit(1)

    current_target = get_current_target(project_dir)
    need_set_target = current_target != target

    suffix = activate_file.suffix.lower()
    system = platform.system().lower()

    if suffix == ".ps1":
        shell = shutil.which("pwsh") or shutil.which("powershell")
        if not shell:
            print("pwsh or powershell was not found", file=sys.stderr)
            sys.exit(1)

        parts = [
            f'& "{activate_file}"',
            f'Set-Location "{project_dir}"',
        ]

        if need_set_target:
            parts.append(f'idf.py set-target {target}')

        parts.append("idf.py build")
        parts.append(f'idf.py -p {port} -b {baud} flash')

        command = "; ".join(parts)

        run_checked([
            shell,
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-Command", command,
        ])
        return

    if suffix in {".bat", ".cmd"}:
        if system != "windows":
            print("BAT/CMD activation scripts can only run on Windows", file=sys.stderr)
            sys.exit(1)

        parts = [
            f'call "{activate_file}"',
            f'cd /d "{project_dir}"',
        ]

        if need_set_target:
            parts.append(f'idf.py set-target {target}')

        parts.append("idf.py build")
        parts.append(f'idf.py -p {port} -b {baud} flash')

        command = " && ".join(parts)
        run_checked(["cmd.exe", "/C", command])
        return

    shell = (
        os.environ.get("SHELL")
        or shutil.which("bash")
        or shutil.which("zsh")
        or shutil.which("sh")
    )

    if not shell:
        print("No shell was found for running the activation script", file=sys.stderr)
        sys.exit(1)

    parts = [
        f'source "{activate_file}"',
        f'cd "{project_dir}"',
    ]

    if need_set_target:
        parts.append(f'idf.py set-target {target}')

    parts.append("idf.py build")
    parts.append(f'idf.py -p {port} -b {baud} flash')

    command = " && ".join(parts)
    run_checked([shell, "-i", "-c", command])


if __name__ == "__main__":
    main()