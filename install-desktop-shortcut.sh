#!/bin/sh
# Adds "Ace Combat 2 (native)" to your applications menu, with its icon.
D="$(dirname "$(readlink -f "$0")")"
EXE="$D/build/ac2native"
[ -x "$D/ac2native" ] && EXE="$D/ac2native"
ICON="$D/assets/icon/ac2.png"
[ -f "$D/icon/ac2.png" ] && ICON="$D/icon/ac2.png"
mkdir -p "$HOME/.local/share/applications"
cat > "$HOME/.local/share/applications/ac2native.desktop" <<EOT
[Desktop Entry]
Type=Application
Name=Ace Combat 2 (native)
Comment=Native PC port of Ace Combat 2
Exec="$EXE" --launcher
Path=$D
Icon=$ICON
StartupWMClass=ac2native
Categories=Game;
EOT
echo "Shortcut installed: $HOME/.local/share/applications/ac2native.desktop"
