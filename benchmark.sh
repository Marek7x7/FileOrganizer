#!/usr/bin/env bash

# Clean previous test folder
TEST_DIR="test_folder"
if [ -d "$TEST_DIR" ]; then
    rm -rf "$TEST_DIR"
fi

# Extensions pool (realistic mix)
EXTENSIONS=(
    ".txt" ".pdf" ".md" ".docx" ".xlsx"   # Documents
    ".jpg" ".png" ".jpeg" ".gif"          # Images
    ".mp4" ".mkv" ".avi"                  # Videos
    ".cpp" ".py" ".js"                    # Code / Other
    ".xyz" ".dat" ".bin"                  # Unknown
)
EXT_COUNT=${#EXTENSIONS[@]}

echo "Creating realistic test structure..."

mkdir -p "$TEST_DIR"

for i in $(seq 1 10); do
    SUB_DIR="$TEST_DIR/sub$i"
    mkdir -p "$SUB_DIR"

    for j in $(seq 1 100); do
        # Pick random extension
        EXT="${EXTENSIONS[$((RANDOM % EXT_COUNT))]}"

        # Occasionally create duplicate names (to test rename logic) ~1 in 10 chance
        if [ $(( (RANDOM % 9) + 1 )) -eq 1 ]; then
            FILE_NAME="duplicate${EXT}"
        else
            FILE_NAME="file_${j}${EXT}"
        fi

        touch "$SUB_DIR/$FILE_NAME"
    done
done

echo "Test structure created."

# Run organizer with timing
echo "Running organizer (recursive)..."

START_NS=$(date +%s%N)
./organizer.exe "$TEST_DIR" --recursive
END_NS=$(date +%s%N)

ELAPSED_NS=$(( END_NS - START_NS ))
ELAPSED_MS=$(( ELAPSED_NS / 1000000 ))
ELAPSED_S=$(echo "scale=6; $ELAPSED_NS / 1000000000" | bc)

# Output results
echo ""
echo "=== RESULT ==="
echo "Total time: ${ELAPSED_MS} ms"
echo "Seconds: ${ELAPSED_S}"
