#!/bin/bash
# Installe, sur un Mac, tout ce qu'il faut pour compiler et envoyer Dash sur l'écran.
#
# Double-clic dans le Finder, ou depuis le dossier du projet :
#   ./installer-mac.command
#   ./installer-mac.command --flash
#
# La première exécution télécharge ESP-IDF 6.1 et ses outils (plusieurs Go).
# Relancer le script ne réinstalle pas ce qui est déjà en place.
# --flash compile puis envoie le firmware sur le port série WCH de l'écran.

set -euo pipefail

IDF_TAG="v6.1"
IDF_DIR="${HOME}/esp/esp-idf-v6.1"
FLASH_ONLY=0
DO_FLASH=0

for arg in "$@"; do
    case "$arg" in
        --flash) DO_FLASH=1 ;;
        -h|--help)
            echo "Usage : $0 [--flash]"
            echo "  sans option   installe les outils, puis propose de flasher"
            echo "  --flash       compile et envoie le firmware sur l'écran"
            exit 0
            ;;
        *)
            echo "Option inconnue : $arg"
            exit 1
            ;;
    esac
done

cd "$(dirname "$0")"
PROJECT_DIR="$(pwd)"

info() { printf '\n==> %s\n' "$1"; }
die() { printf '\nErreur : %s\n' "$1" >&2; exit 1; }

if [ "$(uname -s)" != "Darwin" ]; then
    die "Ce script est prévu pour macOS."
fi

ensure_clt() {
    if xcode-select -p >/dev/null 2>&1; then
        return
    fi
    info "Installation des outils de ligne de commande Xcode"
    xcode-select --install || true
    die "Une fenêtre Xcode vient de s'ouvrir. Termine cette installation, puis relance ce script."
}

ensure_brew() {
    if [ -x /opt/homebrew/bin/brew ]; then
        eval "$(/opt/homebrew/bin/brew shellenv)"
    elif [ -x /usr/local/bin/brew ]; then
        eval "$(/usr/local/bin/brew shellenv)"
    fi
    if command -v brew >/dev/null 2>&1; then
        return
    fi
    info "Installation de Homebrew"
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    if [ -x /opt/homebrew/bin/brew ]; then
        eval "$(/opt/homebrew/bin/brew shellenv)"
    elif [ -x /usr/local/bin/brew ]; then
        eval "$(/usr/local/bin/brew shellenv)"
    else
        die "Homebrew est installé, mais introuvable. Ouvre un nouveau terminal et relance ce script."
    fi
}

ensure_packages() {
    info "Outils de compilation (Python 3.12, CMake, Ninja)"
    brew install python@3.12 cmake ninja dfu-util git
    local py312
    py312="$(brew --prefix python@3.12)/bin/python3.12"
    [ -x "$py312" ] || die "python3.12 est introuvable après l'installation Homebrew."
    local shim="${HOME}/.local/share/dash-idf/py312"
    mkdir -p "$shim"
    ln -sfn "$py312" "${shim}/python3"
    ln -sfn "$py312" "${shim}/python"
    export PATH="${shim}:${PATH}"
    python3 -c 'import sys; raise SystemExit(0 if sys.version_info[:2] == (3, 12) else 1)' \
        || die "python3 n'est pas la version 3.12. ESP-IDF 6.1 en a besoin."
}

ensure_idf() {
    info "ESP-IDF ${IDF_TAG}"
    mkdir -p "${HOME}/esp"
    if [ ! -e "${IDF_DIR}/install.sh" ]; then
        git clone --depth 1 --branch "$IDF_TAG" --recursive --shallow-submodules \
            https://github.com/espressif/esp-idf.git "$IDF_DIR"
    fi
    export IDF_PATH="$IDF_DIR"
    # install.sh recrée l'environnement Python s'il manque. Cible ESP32-S3 seulement.
    ( cd "$IDF_DIR" && ./install.sh esp32s3 )
}

activate_idf() {
    # shellcheck disable=SC1091
    . "${IDF_DIR}/export.sh"
    command -v idf.py >/dev/null 2>&1 || die "idf.py est introuvable après l'activation d'ESP-IDF."
}

prepare_project() {
    info "Configuration du projet pour esp32s3"
    if [ ! -f "${PROJECT_DIR}/sdkconfig" ] || ! grep -q 'CONFIG_IDF_TARGET="esp32s3"' "${PROJECT_DIR}/sdkconfig"; then
        ( cd "$PROJECT_DIR" && idf.py set-target esp32s3 )
    fi
}

pick_port() {
    local ports=()
    local p
    for p in /dev/cu.wchusbserial*; do
        [ -e "$p" ] || continue
        ports+=("$p")
    done
    if [ "${#ports[@]}" -eq 1 ]; then
        printf '%s\n' "${ports[0]}"
        return
    fi
    if [ "${#ports[@]}" -gt 1 ]; then
        echo "Plusieurs écrans sont branchés :" >&2
        local i=1
        for p in "${ports[@]}"; do
            echo "  ${i}) $p" >&2
            i=$((i + 1))
        done
        local choice
        read -r -p "Numéro du port : " choice
        if ! printf '%s' "$choice" | grep -Eq '^[0-9]+$'; then
            die "Choix invalide."
        fi
        if [ "$choice" -lt 1 ] || [ "$choice" -gt "${#ports[@]}" ]; then
            die "Choix invalide."
        fi
        printf '%s\n' "${ports[$((choice - 1))]}"
        return
    fi
    echo "Aucun port WCH trouvé. Ports série visibles :" >&2
    local any=0
    for p in /dev/cu.*; do
        case "$p" in
            /dev/cu.Bluetooth*|/dev/cu.debug-console|/dev/cu.wlan-debug) continue ;;
        esac
        [ -e "$p" ] || continue
        echo "  $p" >&2
        any=1
    done
    [ "$any" -eq 1 ] || echo "  (aucun)" >&2
    die "Branche l'écran (puce WCH), pas l'adaptateur CAN, puis relance avec --flash."
}

flash_firmware() {
    local port
    port="$(pick_port)"
    info "Compilation et envoi sur ${port}"
    ( cd "$PROJECT_DIR" && idf.py -p "$port" flash )
}

ensure_clt
ensure_brew
ensure_packages
ensure_idf
activate_idf
prepare_project

if [ "$DO_FLASH" -eq 0 ] && [ -t 0 ]; then
    echo
    read -r -p "Compiler et envoyer le firmware sur l'écran maintenant ? [o/N] " answer
    case "$answer" in
        o|O|oui|OUI) DO_FLASH=1 ;;
    esac
fi

if [ "$DO_FLASH" -eq 1 ]; then
    flash_firmware
    info "Firmware envoyé. L'écran redémarre."
else
    info "Installation terminée."
    echo "Pour compiler et envoyer plus tard :"
    echo "  cd \"${PROJECT_DIR}\""
    echo "  ./installer-mac.command --flash"
fi
