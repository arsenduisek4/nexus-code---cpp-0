#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"

echo "Сборка Nexus Code..."
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" -j"$(nproc)"

mkdir -p "$PREFIX/bin"
cp "$ROOT/build/nexus" "$PREFIX/bin/nexus"

# ярлык в меню приложений + иконка. ставим в пользовательские каталоги XDG,
# чтобы не требовать root. Exec=nexus берётся из PATH, Terminal=true — TUI-приложение
install -Dm644 "$ROOT/assets/nexus-code.svg" "$HOME/.local/share/icons/nexus-code.svg"
install -Dm644 "$ROOT/assets/nexus-code.desktop" "$HOME/.local/share/applications/nexus-code.desktop"
update-desktop-database "$HOME/.local/share/applications" >/dev/null 2>&1 || true
echo "Ярлык установлен в меню приложений."

mkdir -p "$HOME/.nexuscode/logs"
if [ ! -f "$HOME/.nexuscode/config.json" ]; then
    cp "$ROOT/config/config.example.json" "$HOME/.nexuscode/config.json"
    echo "Создан конфиг ~/.nexuscode/config.json — впишите свой API-ключ."
fi

echo "Установлено: $PREFIX/bin/nexus"
case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) echo "Добавьте в PATH:  export PATH=\"$PREFIX/bin:\$PATH\"" ;;
esac
