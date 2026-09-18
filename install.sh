#!/usr/bin/env sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Execute este instalador como root: sudo ./install.sh" >&2
    exit 1
fi

project_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$project_root"

make
make install
systemctl daemon-reload
systemctl enable --now cpu-clock-switch.service

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/share/applications || true
fi

echo "CPU Switch Control instalado. Execute: cpu-switch-control"
