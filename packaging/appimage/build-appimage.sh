#!/bin/sh

# CPU Switch Control - gerador do AppImage.
#
# Compila o projeto, monta o AppDir com linuxdeploy, inclui daemon/configuração
# e produz o AppImage x86_64 validado pelo workflow do GitHub Actions.

set -eu

# Resolve caminhos a partir do próprio script para funcionar de qualquer diretório atual.
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
APPDIR="${APPDIR:-$ROOT_DIR/AppDir}"
DIST_DIR="${DIST_DIR:-$ROOT_DIR/dist}"
TOOLS_DIR="${TOOLS_DIR:-$ROOT_DIR/tools}"
LINUXDEPLOY="${LINUXDEPLOY:-$TOOLS_DIR/linuxdeploy-x86_64.AppImage}"
GTK_PLUGIN="${GTK_PLUGIN:-$TOOLS_DIR/linuxdeploy-plugin-gtk.sh}"
APPIMAGETOOL="${APPIMAGETOOL:-$TOOLS_DIR/appimagetool-x86_64.AppImage}"
OUTPUT="${OUTPUT:-$DIST_DIR/CPU-Switch-Control-x86_64.AppImage}"

# Falha cedo com mensagem clara se o ambiente de empacotamento não foi preparado.
for tool in "$LINUXDEPLOY" "$GTK_PLUGIN" "$APPIMAGETOOL"; do
    [ -x "$tool" ] || {
        echo "Ferramenta de empacotamento ausente ou não executável: $tool" >&2
        exit 1
    }
done

# Sempre empacota binários recém-compilados da mesma revisão do código.
make -C "$ROOT_DIR" all

rm -rf "$APPDIR"
mkdir -p "$APPDIR" "$DIST_DIR"

# linuxdeploy locates plugins next to its own executable or in PATH.
export PATH="$TOOLS_DIR:$PATH"
export APPIMAGE_EXTRACT_AND_RUN="${APPIMAGE_EXTRACT_AND_RUN:-1}"

"$LINUXDEPLOY"     --appdir "$APPDIR"     --executable "$ROOT_DIR/build/cpu-switch-control"     --executable "$ROOT_DIR/build/cpu-clock-switch-daemon"     --desktop-file "$ROOT_DIR/packaging/appimage/cpu-switch-control.desktop"     --icon-file "$ROOT_DIR/assets/cpu-switch-control.svg"     --plugin gtk

# Keep privileged/system integration resources inside the immutable AppImage.
# Recursos abaixo não são dependências ELF; precisam ser copiados explicitamente para o AppDir.
install -Dm755 "$ROOT_DIR/build/cpu-clock-switch-daemon"     "$APPDIR/usr/bin/cpu-clock-switch-daemon"
install -Dm755 "$ROOT_DIR/packaging/appimage/install-system-components.sh"     "$APPDIR/usr/lib/cpu-switch-control/install-system-components.sh"
install -Dm644 "$ROOT_DIR/service/cpu-clock-switch.service"     "$APPDIR/usr/share/cpu-switch-control/cpu-clock-switch.service"
install -Dm644 "$ROOT_DIR/config/cpu-clock-switch.json"     "$APPDIR/usr/share/cpu-switch-control/cpu-clock-switch.json"

# Replace linuxdeploy's generic launcher after its GTK hook has been installed.
# Usa nosso launcher após o plugin ter criado seus hooks de runtime.
install -Dm755 "$ROOT_DIR/packaging/appimage/AppRun" "$APPDIR/AppRun"

rm -f "$OUTPUT"
# appimagetool transforma o AppDir completo no arquivo final distribuível.
ARCH=x86_64 "$APPIMAGETOOL" "$APPDIR" "$OUTPUT"

echo "AppImage criado em: $OUTPUT"
