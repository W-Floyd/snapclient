#!/usr/bin/env bash
set -euo pipefail

ESP_IDF_VERSION="v5.5.1"
IMAGE="espressif/idf:${ESP_IDF_VERSION}"
TARGETS=("esp32" "esp32s3")

echo "Checking submodules..."
git submodule update --init --recursive

echo "Pulling ${IMAGE}..."
docker pull "${IMAGE}"

PASS=()
FAIL=()

for TARGET in "${TARGETS[@]}"; do
    echo ""
    echo "=== Building for ${TARGET} ==="
    if docker run --rm \
        -v "$(pwd)":/project \
        -w /project \
        "${IMAGE}" \
        bash -c "idf.py set-target ${TARGET} build"; then
        PASS+=("${TARGET}")
    else
        FAIL+=("${TARGET}")
        rm -rf build sdkconfig
        echo ""
        echo "=== Results ==="
        for T in "${PASS[@]+"${PASS[@]}"}"; do echo "  PASS  ${T}"; done
        for T in "${FAIL[@]+"${FAIL[@]}"}"; do echo "  FAIL  ${T}"; done
        exit 1
    fi

    # Remove build artifacts so next target starts clean
    rm -rf build sdkconfig
done

echo ""
echo "=== Results ==="
for T in "${PASS[@]+"${PASS[@]}"}"; do echo "  PASS  ${T}"; done
for T in "${FAIL[@]+"${FAIL[@]}"}"; do echo "  FAIL  ${T}"; done

if [ ${#FAIL[@]} -ne 0 ]; then
    exit 1
fi
