#!/usr/bin/env bash
set -euo pipefail

repo="asrma7/waybar-earbud"
version="${EARBUD_BATTERY_VERSION:-latest}"
prefix="${PREFIX:-/usr/local}"
bindir="${BINDIR:-}"
uninstall=false

usage() {
  cat <<'EOF'
Usage: install.sh [--version tag] [--prefix path] [--bindir path] [--uninstall]

Installs waybar-earbud from GitHub release assets.

Options:
  --version tag       Release tag to install. Defaults to latest.
  --prefix path       Install prefix. Defaults to /usr/local.
  --bindir path       Binary install directory. Defaults to <prefix>/bin.
  --uninstall         Remove waybar-earbud from the install directory.
  -h, --help          Show this help.

Environment:
  EARBUD_BATTERY_VERSION   Same as --version.
  PREFIX                   Same as --prefix.
  BINDIR                   Same as --bindir.
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --version)
      version="${2:?missing value for --version}"
      shift 2
      ;;
    --prefix)
      prefix="${2:?missing value for --prefix}"
      shift 2
      ;;
    --bindir)
      bindir="${2:?missing value for --bindir}"
      shift 2
      ;;
    --uninstall)
      uninstall=true
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "install.sh: unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

case "$(uname -m)" in
  x86_64|amd64)
    arch="x86_64"
    ;;
  aarch64|arm64)
    arch="aarch64"
    ;;
  *)
    echo "install.sh: unsupported architecture: $(uname -m)" >&2
    exit 1
    ;;
esac

if [ -z "$bindir" ]; then
  bindir="$prefix/bin"
fi

if [ "$uninstall" = true ]; then
  target="$bindir/waybar-earbud"
  if [ ! -e "$target" ]; then
    echo "Not installed: $target"
    exit 0
  fi

  if [ -w "$target" ] || [ -w "$(dirname "$target")" ]; then
    rm -f "$target"
  else
    sudo rm -f "$target"
  fi

  echo "Removed: $target"
  exit 0
fi

asset="waybar-earbud-linux-${arch}.tar.gz"
if [ "$version" = "latest" ]; then
  url="https://github.com/${repo}/releases/latest/download/${asset}"
else
  url="https://github.com/${repo}/releases/download/${version}/${asset}"
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

download_to="$tmpdir/$asset"
echo "Downloading $url"
if command -v curl >/dev/null 2>&1; then
  curl -fL "$url" -o "$download_to"
elif command -v wget >/dev/null 2>&1; then
  wget -O "$download_to" "$url"
else
  echo "install.sh: curl or wget is required" >&2
  exit 1
fi

tar -xzf "$download_to" -C "$tmpdir"
binary="$(find "$tmpdir" -type f -name waybar-earbud -perm -111 | head -n 1)"
if [ -z "$binary" ]; then
  echo "install.sh: release archive did not contain waybar-earbud" >&2
  exit 1
fi

if mkdir -p "$bindir" 2>/dev/null && [ -w "$bindir" ]; then
  install -Dm755 "$binary" "$bindir/waybar-earbud"
else
  sudo install -Dm755 "$binary" "$bindir/waybar-earbud"
fi

cat <<EOF
Installed: $bindir/waybar-earbud

Runtime dependencies still need to be available on the system:
  bluez/libbluetooth, glib/gio, and pactl for default audio output detection.
EOF
