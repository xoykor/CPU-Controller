#!/bin/sh

# CPU Switch Control - preparação da integração systemd para o AppImage.
#
# O mount FUSE de um AppImage pertence ao usuário que o abriu. Um processo
# iniciado por pkexec pode não conseguir ler arquivos diretamente desse mount.
# Por isso copiamos primeiro os componentes necessários para um diretório
# temporário normal e só então elevamos o instalador.
#
# O diretório criado por mktemp é privado do usuário e é removido ao terminar.

set -eu

if [ -n "${CPU_SWITCH_APPDIR:-}" ]; then
    APPROOT="$CPU_SWITCH_APPDIR"
else
    SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
    APPROOT=$(CDPATH= cd -- "$SELF_DIR/../../.." && pwd)
fi

DAEMON="$APPROOT/usr/bin/cpu-clock-switch-daemon"
UNIT="$APPROOT/usr/share/cpu-switch-control/cpu-clock-switch.service"
DEFAULT_CONFIG="$APPROOT/usr/share/cpu-switch-control/cpu-clock-switch.json"
INSTALLER="$APPROOT/usr/lib/cpu-switch-control/install-system-components.sh"

# Se a versão instalada já coincide com o AppImage, não pede autorização.
need_setup=0
if [ ! -x /usr/local/bin/cpu-clock-switch-daemon ] ||
   ! cmp -s "$DAEMON" /usr/local/bin/cpu-clock-switch-daemon; then
    need_setup=1
fi
if [ ! -f /etc/systemd/system/cpu-clock-switch.service ] ||
   ! cmp -s "$UNIT" /etc/systemd/system/cpu-clock-switch.service; then
    need_setup=1
fi

if [ "$need_setup" -eq 0 ]; then
    exit 0
fi

command -v pkexec >/dev/null 2>&1 || {
    echo "pkexec não está disponível; não foi possível instalar o serviço." >&2
    exit 1
}

for required in "$DAEMON" "$UNIT" "$DEFAULT_CONFIG" "$INSTALLER"; do
    [ -e "$required" ] || {
        echo "Componente embutido ausente: $required" >&2
        exit 1
    }
done

STAGING_DIR=$(mktemp -d "${TMPDIR:-/tmp}/cpu-switch-control.XXXXXX")
cleanup() {
    rm -rf "$STAGING_DIR"
}
trap cleanup EXIT HUP INT TERM

chmod 700 "$STAGING_DIR"
cp "$DAEMON" "$STAGING_DIR/cpu-clock-switch-daemon"
cp "$UNIT" "$STAGING_DIR/cpu-clock-switch.service"
cp "$DEFAULT_CONFIG" "$STAGING_DIR/cpu-clock-switch.json"
cp "$INSTALLER" "$STAGING_DIR/install-system-components.sh"

chmod 755 "$STAGING_DIR/cpu-clock-switch-daemon"
chmod 755 "$STAGING_DIR/install-system-components.sh"
chmod 644 "$STAGING_DIR/cpu-clock-switch.service"
chmod 644 "$STAGING_DIR/cpu-clock-switch.json"

# O instalador elevado lê apenas os arquivos já copiados para o staging.
pkexec "$STAGING_DIR/install-system-components.sh"
