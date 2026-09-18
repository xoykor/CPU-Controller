#!/bin/sh

# CPU Switch Control - instalador privilegiado dos componentes do sistema.
#
# Este arquivo é executado a partir de um diretório temporário preparado pelo
# usuário normal. Ele não recebe destinos nem comandos arbitrários: instala
# somente os quatro caminhos fixos conhecidos pelo projeto.

set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Este instalador precisa ser executado via pkexec/root." >&2
    exit 126
fi

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

DAEMON="$SELF_DIR/cpu-clock-switch-daemon"
UNIT="$SELF_DIR/cpu-clock-switch.service"
DEFAULT_CONFIG="$SELF_DIR/cpu-clock-switch.json"

[ -x "$DAEMON" ] || { echo "Daemon temporário não encontrado." >&2; exit 1; }
[ -f "$UNIT" ] || { echo "Unidade systemd temporária não encontrada." >&2; exit 1; }
[ -f "$DEFAULT_CONFIG" ] || { echo "Configuração padrão temporária não encontrada." >&2; exit 1; }

/usr/bin/install -Dm755 "$DAEMON" /usr/local/bin/cpu-clock-switch-daemon
/usr/bin/install -Dm644 "$UNIT" /etc/systemd/system/cpu-clock-switch.service

# Nunca sobrescreve preferências existentes do usuário.
if [ ! -e /etc/cpu-clock-switch.json ]; then
    /usr/bin/install -Dm644 "$DEFAULT_CONFIG" /etc/cpu-clock-switch.json
fi

/usr/bin/systemctl daemon-reload
/usr/bin/systemctl enable cpu-clock-switch.service
/usr/bin/systemctl restart cpu-clock-switch.service
