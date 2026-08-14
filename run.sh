#!/usr/bin/env bash
set -euo pipefail

# Prefer the Python-installed CMake tools if present.
if [ -d "$HOME/Library/Python/3.14/bin" ]; then
  export PATH="$HOME/Library/Python/3.14/bin:$PATH"
fi

MODE="${1:-run}"

case "$MODE" in
  run)
    cmake -S . -B build
    cmake --build build
    ./build/bin/cfd_sim
    ;;
  test)
    cmake -S . -B build
    cmake --build build
    ctest --test-dir build --output-on-failure
    ;;
  all)
    cmake -S . -B build
    cmake --build build
    ctest --test-dir build --output-on-failure
    ./build/bin/cfd_sim
    ;;
  *)
    echo "Usage: ./run.sh [run|test|all]"
    echo "  run   configure + build + launch the app (default)"
    echo "  test  configure + build + run the test suite"
    echo "  all   configure + build + test + launch the app"
    exit 1
    ;;
esac
