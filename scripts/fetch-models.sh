#!/usr/bin/env bash
# Fetch whisper.cpp ggml models for hypxrvoice's ASR tier.
#
# Models are NOT committed (they are large binaries). Run this once, then point
# asr.model in your config.toml at the downloaded file.
#
# Usage:
#   scripts/fetch-models.sh [model ...]   # default: base.en
#   scripts/fetch-models.sh tiny.en base.en small.en
#
# Destination: $HYPXRVOICE_MODEL_DIR, else ./models next to the repo.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${HYPXRVOICE_MODEL_DIR:-$REPO_DIR/models}"
BASE_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main"

# sha256 checksums that this repo has verified locally. Models not listed here are
# still downloaded, but only warned (not hard-failed) on checksum — verify upstream
# before trusting them.
declare -A SHA256=(
  ["ggml-base.en.bin"]="a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002"
)

models=("$@")
[ ${#models[@]} -eq 0 ] && models=("base.en")

mkdir -p "$DEST"
for m in "${models[@]}"; do
  file="ggml-${m}.bin"
  out="$DEST/$file"
  if [ -f "$out" ]; then
    echo "[skip] $file already present at $out"
  else
    echo "[fetch] $file -> $out"
    curl -fSL --progress-bar -o "$out.tmp" "$BASE_URL/$file"
    mv "$out.tmp" "$out"
  fi
  want="${SHA256[$file]:-}"
  got="$(sha256sum "$out" | cut -d' ' -f1)"
  if [ -n "$want" ]; then
    if [ "$want" = "$got" ]; then
      echo "[ok]   $file sha256 verified"
    else
      echo "[FAIL] $file sha256 mismatch: want $want got $got" >&2
      exit 1
    fi
  else
    echo "[warn] $file has no pinned checksum in this script; sha256=$got (verify upstream)"
  fi
  echo "       set in config.toml:  asr.model = \"$out\""
done

echo
echo "Recommended default: base.en (good accuracy, ~142MB). The research doc favors"
echo "'small' for tougher phrasing (~466MB); tiny.en (~75MB) is fastest/least accurate."
