#!/bin/bash
# Fetch reference sources for the Tegra 3D (gr3d) work into <repo>/ext (MIT/Mesa licensed).
set -e
cd "$(dirname "$0")/.." && mkdir -p ext && cd ext
[ -d grate ] || git clone --depth 1 https://github.com/grate-driver/grate.git
if [ ! -d grate-mesa ]; then
  git clone --depth 1 --filter=blob:none --sparse https://github.com/grate-driver/mesa.git grate-mesa
  cd grate-mesa
  git sparse-checkout set src/gallium/drivers/tegra src/gallium/drivers/grate
  cd ..
fi
ls grate grate-mesa/src/gallium/drivers
