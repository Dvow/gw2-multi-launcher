#!/usr/bin/env bash
# Package smoke test for a disposable Debian/Ubuntu or Arch container.
set -euo pipefail
package=$(realpath "${1:?Pass a .deb or .pkg.tar.zst path}")
case "$package" in
  *.deb)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends --reinstall "$package" xvfb xauth libgl1-mesa-dri dbus-x11 x11-utils
    name=$(dpkg-deb -f "$package" Package)
    format=DEB
    ;;
  *.pkg.tar.zst)
    pacman -Syu --noconfirm --needed xorg-server-xvfb xorg-xauth mesa dbus xorg-xwininfo
    pacman -U --noconfirm "$package"
    name=$(pacman -Qp "$package" | cut -d ' ' -f 1)
    format=ARCH
    ;;
  *) echo 'Unsupported package format' >&2; exit 1 ;;
esac
test "$(cat "/usr/lib/$name/package-format")" = "$format"
for proxy_script in kx_proxy.py kx_proxy_linux.py kx_proxy_tun.py; do
  test -s "/usr/lib/$name/$proxy_script"
done
expected=$(sed -n 's/^Name=//p' "/usr/share/applications/$name.desktop")
test -n "$expected"
test_user="launcher-test-$$"
useradd --create-home --shell /bin/bash "$test_user"
runtime="/run/user/$(id -u "$test_user")"
install -d -m 700 -o "$test_user" -g "$(id -g "$test_user")" "$runtime"
log="$runtime/launcher-smoke.log"
runuser -u "$test_user" -- env -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR="$runtime" \
  dbus-run-session -- xvfb-run -a env LIBGL_ALWAYS_SOFTWARE=1 bash -c '
  set -e
  "$1" > "$XDG_RUNTIME_DIR/launcher-smoke.log" 2>&1 & pid=$!
  trap '\''kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true'\'' EXIT
  sleep 5
  kill -0 "$pid"
  xwininfo -root -tree > "$XDG_RUNTIME_DIR/launcher-windows.log"
  cat "$XDG_RUNTIME_DIR/launcher-windows.log"
  grep -F "\"$2\"" "$XDG_RUNTIME_DIR/launcher-windows.log"
' smoke "$name" "$expected"
cat "$log"
if grep -Eqi 'error while loading shared libraries|symbol lookup error|segmentation fault|failed to create|could not initialize' "$log"; then
  exit 1
fi
echo "Package dependencies, executable startup and fresh desktop smoke test passed: $(. /etc/os-release; echo "$PRETTY_NAME")"
