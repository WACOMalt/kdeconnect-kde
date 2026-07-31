#!/bin/bash
set -e
mkdir -p /output
# Log everything to a file on the mounted volume; the docker stdout redirect
# through WSL proved unreliable for capturing the early steps.
exec > >(tee /output/build.log) 2>&1

export LANG=en_US.UTF-8

echo "=== PREP: system deps not in the CI image ==="
dnf install -y epel-release 2>&1 | tail -2
dnf install -y meson ninja-build libevdev-devel libxkbcommon-devel systemd-devel python3-jinja2 \
    libfakekey-devel libXtst-devel 2>&1 | tail -3

# libei isn't packaged for AlmaLinux 9 / EPEL, but plugins/mousepad requires it
# unconditionally on Linux, so build it from source.
git clone --depth=1 https://gitlab.freedesktop.org/libinput/libei.git /tmp/libei 2>&1 | tail -2
cd /tmp/libei
meson setup builddir --prefix=/usr -Dtests=disabled -Ddocumentation=[] 2>&1 | tail -3
ninja -C builddir install 2>&1 | tail -3
echo "libei version: $(pkg-config --modversion libei-1.0)"

echo "=== CLONE CRAFT TOOLING ==="
mkdir -p /work && cd /work
git clone https://invent.kde.org/packaging/craftmaster.git --branch=master 2>&1 | tail -2
git clone https://invent.kde.org/sysadmin/craft-ci.git --depth=1 2>&1 | tail -2
touch /src/.craft.ini

export KDECI_CRAFT_PLATFORM=linux-64-gcc
export KDECI_CRAFT_CONFIG=craft-ci/qt6/CraftConfig.ini
export KDECI_CRAFT_PROJECT_CONFIG=/src/.craft.ini
export KDECI_PYTHON_CMD=python3.11
craftmaster() { $KDECI_PYTHON_CMD /work/craftmaster/CraftMaster.py --config $KDECI_CRAFT_CONFIG --config-override $KDECI_CRAFT_PROJECT_CONFIG --target $KDECI_CRAFT_PLATFORM "$@"; }

export PKG_CONFIG_PATH=$(/usr/bin/pkg-config --variable pc_path pkg-config)
source /opt/rh/gcc-toolset-14/enable

echo "=== SETUP ==="
craftmaster --setup

echo "=== UPDATE CRAFT (this is what clones craft-blueprints-kde) ==="
craftmaster -c -i --options virtual.ignored=True --update craft

echo "=== WRITE CUSTOM AppRun ==="
# KDE Connect's daemon is normally launched via D-Bus activation. That cannot work
# from an AppImage: the service file bundled inside it records the build-time path
# (/work/linux-64-gcc/bin/kdeconnectd), and the session bus - started long before
# the AppImage runs - never scans the bundle's service directory anyway. So the
# daemon has to be started explicitly here, or nothing in the app functions.
cat > /work/custom-AppRun <<'APPRUN'
#!/usr/bin/env bash
this_dir="$(readlink -f "$(dirname "$0")")"

# Environment set up by craft (XDG_DATA_DIRS/PATH) and linuxdeploy's Qt plugin.
for hook in "$this_dir"/apprun-hooks/*.sh; do
    [ -r "$hook" ] && source "$hook"
done

bindir="$this_dir/usr/bin"
logdir="${XDG_STATE_HOME:-$HOME/.local/state}/kdeconnect-appimage"
mkdir -p "$logdir" 2>/dev/null

ensure_icons() {
    # The StatusNotifierItem protocol sends only an icon *name* over D-Bus; the
    # panel/tray applet then resolves that name against its own icon theme. Our
    # icons live inside the AppImage mount, which the panel process cannot see,
    # so the tray entry renders as a broken image. Copy them into the user's
    # icon theme once so the host can find them.
    local src="$this_dir/usr/share/icons/hicolor/scalable/apps"
    local dst="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/scalable/apps"
    [ -d "$src" ] || return 0
    [ -e "$dst/kdeconnectindicatordark.svg" ] && return 0
    mkdir -p "$dst" 2>/dev/null || return 0
    cp -n "$src"/kdeconnect*.svg "$dst"/ 2>/dev/null
    gtk-update-icon-cache -f -t "${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor" >/dev/null 2>&1 || true
}

ensure_daemon() {
    # Only start our bundled daemon if one isn't already running (e.g. a
    # distro-packaged KDE Connect, or a previous run of this AppImage).
    if ! pgrep -x kdeconnectd >/dev/null 2>&1; then
        # Keep the daemon's output: it is the only place connection/pairing
        # failures show up, and discarding it makes problems undiagnosable.
        "$bindir/kdeconnectd" >>"$logdir/daemon.log" 2>&1 &
        # Give it a moment to claim its name on the session bus.
        sleep 1
    fi
}

case "$1" in
    --cli)       shift; ensure_daemon; exec "$bindir/kdeconnect-cli" "$@" ;;
    --app)       shift; ensure_icons; ensure_daemon; exec "$bindir/kdeconnect-app" "$@" ;;
    --sms)       shift; ensure_daemon; exec "$bindir/kdeconnect-sms" "$@" ;;
    --indicator) shift; ensure_icons; ensure_daemon; exec "$bindir/kdeconnect-indicator" "$@" ;;
    --daemon)    shift; exec "$bindir/kdeconnectd" "$@" ;;
    --help|-h)
        cat <<'USAGE'
KDE Connect AppImage

  (no arguments)  Start the daemon and tray indicator, and open the main window.
  --app           Open the main window only.
  --indicator     Tray indicator only.
  --cli  [args]   Run kdeconnect-cli (e.g. --cli --list-devices).
  --sms           Open the SMS app.
  --daemon        Run the daemon in the foreground.

Logs are written to ${XDG_STATE_HOME:-$HOME/.local/state}/kdeconnect-appimage/.
USAGE
        exit 0
        ;;
esac

ensure_icons
ensure_daemon

# Keep a tray icon around so the connection persists after the window is closed.
if ! pgrep -x kdeconnect-indicator >/dev/null 2>&1; then
    "$bindir/kdeconnect-indicator" >>"$logdir/indicator.log" 2>&1 &
fi

exec "$bindir/kdeconnect-app" "$@"
APPRUN
chmod +x /work/custom-AppRun

echo "=== PATCH BLUEPRINT: point AppImage packager at the real desktop file ==="
# The blueprint sets appname=kdeconnect-indicator, but the desktop file that
# actually gets installed is org.kde.kdeconnect.nonplasma.desktop, so
# AppImagePackager's glob ("*<appname>.desktop") matches nothing and packaging
# aborts. AppImagePackager honours a 'desktopFile' define that overrides appname
# for exactly this lookup. (kdeconnect-kde has no AppImage job in its CI, so this
# code path was never exercised upstream.)
BP=$(find /work -path "*craft-blueprints-kde/kde/applications/kdeconnect-kde/kdeconnect-kde.py" | head -1)
if [ -z "$BP" ]; then
    echo "FATAL: could not locate kdeconnect-kde blueprint" >&2
    find /work -name "kdeconnect-kde.py" >&2 || true
    exit 1
fi
echo "blueprint: $BP"
python3.11 - "$BP" <<'PYEOF'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
s = p.read_text()
old = '            self.defines["appname"] = "kdeconnect-indicator"\n'
new = (old
       + '            self.defines["desktopFile"] = "org.kde.kdeconnect.nonplasma"\n'
       + '            self.defines["appimage_apprun"] = "/work/custom-AppRun"\n')
if 'desktopFile' in s:
    print("already patched, skipping")
else:
    assert old in s, "anchor line not found in blueprint"
    p.write_text(s.replace(old, new, 1))
    print("patched OK")
PYEOF

echo "=== INSTALL DEPS ==="
craftmaster -c --install-deps kdeconnect-kde

echo "=== BUILD ==="
craftmaster -c --no-cache --options kdeconnect-kde.srcDir=/src kdeconnect-kde

echo "=== INSTALL LINUXDEPLOY ==="
craftmaster -c -i --update linuxdeploy

echo "=== PACKAGE ==="
craftmaster -c --package --options kdeconnect-kde.srcDir=/src kdeconnect-kde

echo "=== COPY OUT ==="
packageDir=$(craftmaster -c -q --get "packageDestinationDir()" virtual/base)
echo "packageDir=$packageDir"
ls -la "$packageDir"
cp -v "$packageDir"/*.AppImage /output/
echo "=== DONE ==="
ls -la /output/
