#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
binary="$project_root/target/release/cpu-switch-control"

if [[ "${EUID}" -ne 0 ]]; then
    echo "Execute este instalador como root: sudo ./install.sh" >&2
    exit 1
fi

if [[ ! -x "$binary" ]]; then
    echo "Binário não encontrado. Execute antes: cargo build --release" >&2
    exit 1
fi

install -Dm755 "$binary" /usr/local/bin/cpu-switch-control
install -Dm755 "$project_root/service/cpu-clock-switch.py" /usr/local/bin/cpu-clock-switch.py
install -Dm644 "$project_root/service/cpu-clock-switch.service" /etc/systemd/system/cpu-clock-switch.service
install -Dm644 "$project_root/assets/cpu-switch-control.svg" /usr/share/icons/hicolor/scalable/apps/cpu-switch-control.svg
install -Dm644 "$project_root/packaging/cpu-switch-control.desktop" /usr/share/applications/cpu-switch-control.desktop
if [[ ! -e /etc/cpu-clock-switch.json ]]; then
    install -Dm644 "$project_root/config/cpu-clock-switch.json" /etc/cpu-clock-switch.json
fi

systemctl daemon-reload
systemctl enable --now cpu-clock-switch.service
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/share/applications || true
fi
echo "CPU Switch Control instalado. Execute: cpu-switch-control"
