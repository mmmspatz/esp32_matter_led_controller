#!/usr/bin/env bash
# Generate the MCUboot OTA image-signing key if it does not exist yet.
#
#   ./scripts/gen-signing-key.sh
#
# The key is a per-developer secret (keys/ is gitignored), so a fresh clone has
# none and the build fails at the signing step. bootstrap.sh runs this; it is
# also safe to run by hand after `source activate.sh`.
#
# Idempotent by design: an existing key is never overwritten. Regenerating is a
# deliberate act -- delete the file first -- because the public half is baked
# into the MCUboot binary at build time, so a new key orphans every board still
# running a bootloader built from the old one (USB reflash to recover).

set -e

for arg in "$@"; do
    case "$arg" in
        -h|--help)
            sed -n '2,4p' "$0" | sed 's/^# \?//'
            exit 0 ;;
        *) echo "unknown flag: $arg" >&2; exit 2 ;;
    esac
done

TOPDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# app/sysbuild.conf is the single source of truth for the path: the build reads
# that symbol, so parse it rather than duplicating the filename here.
# shellcheck disable=SC2016  # ${WEST_TOPDIR} is literal text in the sed pattern
KEY_REL=$(sed -n \
    's/^SB_CONFIG_BOOT_SIGNATURE_KEY_FILE="\${WEST_TOPDIR}\/\(.*\)"[[:space:]]*$/\1/p' \
    "$TOPDIR/app/sysbuild.conf")
if [ -z "$KEY_REL" ]; then
    echo "error: no \${WEST_TOPDIR}-relative SB_CONFIG_BOOT_SIGNATURE_KEY_FILE" \
         "in app/sysbuild.conf; this script needs updating" >&2
    exit 1
fi
KEY="$TOPDIR/$KEY_REL"

# The key type must match SB_CONFIG_BOOT_SIGNATURE_TYPE_* in the same file.
if grep -q '^SB_CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y' "$TOPDIR/app/sysbuild.conf"; then
    KEY_TYPE=ecdsa-p256
else
    echo "error: app/sysbuild.conf no longer selects ECDSA-P256 signing;" \
         "this script needs updating" >&2
    exit 1
fi

IMGTOOL=$(command -v imgtool || true)
if [ -z "$IMGTOOL" ] && [ -x "$TOPDIR/.venv-zephyr/bin/imgtool" ]; then
    IMGTOOL="$TOPDIR/.venv-zephyr/bin/imgtool"
fi
if [ -z "$IMGTOOL" ]; then
    echo "error: imgtool not found — run ./bootstrap.sh, or 'source activate.sh'" >&2
    exit 1
fi

# Short digest of the public half: identifies which key a bootloader was built
# with, so a board that rejects an OTA image can be matched against the key on
# disk without unpacking the MCUboot binary.
fingerprint() {
    "$IMGTOOL" getpub -k "$1" -e raw | sha256sum | cut -c1-16
}

if [ -f "$KEY" ]; then
    echo "signing key present: $KEY_REL (pubkey $(fingerprint "$KEY"))"
    exit 0
fi

mkdir -p "$(dirname "$KEY")"
"$IMGTOOL" keygen -k "$KEY" -t "$KEY_TYPE"
chmod 600 "$KEY"

cat <<EOF

  Generated a new $KEY_TYPE OTA signing key:

      $KEY_REL   (pubkey $(fingerprint "$KEY"))

  BACK IT UP, and keep it secret. MCUboot embeds the public half at build
  time and validates every OTA image against it, so losing this file means
  the boards you flash from this checkout can never be updated over the air
  again -- only by USB reflash.

EOF
