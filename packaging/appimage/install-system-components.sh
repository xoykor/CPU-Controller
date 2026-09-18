#!/bin/sh

# CPU Switch Control - instalador privilegiado usado pelo AppImage.
#
# Este script é chamado via pkexec. Ele instala apenas destinos fixos do projeto
# e nunca aceita caminhos arbitrários fornecidos pelo usuário.

set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Este instalador precisa ser executado via pkexec/root." >&2
    exit 126
fi

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APPROOT=$(CDPATH= cd -- "$SELF_DIR/../../.." && pwd)

DAEMON="$APPROOT/usr/bin/cpu-clock-switch-daemon"
UNIT="$APPROOT/usr/share/cpu-switch-control/cpu-clock-switch.service"
DEFAULT_CONFIG="$APPROOT/usr/share/cpu-switch-control/cpu-clock-switch.json"

[ -x "$DAEMON" ] || { echo "Daemon embutido não encontrado." >&2; exit 1; }
[ -f "$UNIT" ] || { echo "Unidade systemd embutida não encontrada." >&2; exit 1; }
[ -f "$DEFAULT_CONFIG" ] || { echo "Configuração padrão embutida não encontrada." >&2; exit 1; }

/usr/bin/install -Dm755 "$DAEMON" /usr/local/bin/cpu-clock-switch-daemon
/usr/bin/install -Dm644 "$UNIT" /etc/systemd/system/cpu-clock-switch.service

if [ ! -e /etc/cpu-clock-switch.json ]; then
    /usr/bin/install -Dm644 "$DEFAULT_CONFIG" /etc/cpu-clock-switch.json
fi

/usr/bin/systemctl daemon-reload
/usr/bin/systemctl enable cpu-clock-switch.service
/usr/bin/systemctl restart cpu-clock-switch.service
