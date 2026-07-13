#!/usr/bin/env bash
# Fetch models for hypxrvoice: whisper.cpp ggml (ASR tier) + an optional GGUF instruct
# model (the local-LLM intent backend, WP-V4).
#
# Models are NOT committed (they are large binaries). Run this once, then point
# asr.model / intent.model in your config.toml at the downloaded file(s).
#
# Usage:
#   scripts/fetch-models.sh [model ...]   # default: base.en
#   scripts/fetch-models.sh tiny.en base.en small.en
#   scripts/fetch-models.sh intent        # the recommended 3B-class GGUF intent model
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

# ---- WP-V4 intent GGUF (local-LLM backend). Pinned URL + sha256. ----
INTENT_FILE="Qwen2.5-3B-Instruct-Q4_K_M.gguf"
INTENT_URL="https://huggingface.co/bartowski/Qwen2.5-3B-Instruct-GGUF/resolve/main/${INTENT_FILE}"
INTENT_SHA256="9c9f56a391a3abbd5b89d0245bf6106081bcc3173119d4229235dd9d23253f94"

fetch_intent() {
  local out="$DEST/$INTENT_FILE"
  if [ -f "$out" ]; then
    echo "[skip] $INTENT_FILE already present at $out"
  else
    echo "[fetch] $INTENT_FILE (~1.9GB) -> $out"
    curl -fSL --retry 3 --progress-bar -o "$out.tmp" "$INTENT_URL"
    mv "$out.tmp" "$out"
  fi
  local got; got="$(sha256sum "$out" | cut -d' ' -f1)"
  if [ "$got" = "$INTENT_SHA256" ]; then
    echo "[ok]   $INTENT_FILE sha256 verified"
  else
    echo "[FAIL] $INTENT_FILE sha256 mismatch: want $INTENT_SHA256 got $got" >&2
    exit 1
  fi
  echo "       set in config.toml:  [intent] backend=\"llama\"  model = \"$out\""
}

models=("$@")
[ ${#models[@]} -eq 0 ] && models=("base.en")

mkdir -p "$DEST"
for m in "${models[@]}"; do
  # The GGUF intent model is fetched via its own pinned spec.
  if [ "$m" = "intent" ] || [ "$m" = "llm" ]; then
    fetch_intent
    continue
  fi
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
