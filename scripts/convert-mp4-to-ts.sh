#!/bin/bash
# Convert all MP4 files in a folder to MPEG-TS format
# Usage: ./convert-mp4-to-ts.sh [input_folder] [output_folder]

set -e

INPUT_DIR="${1:-.}"
OUTPUT_DIR="${2:-$INPUT_DIR}"

# Check for ffmpeg
if ! command -v ffmpeg &> /dev/null; then
    echo "Error: ffmpeg not found. Please install ffmpeg first."
    exit 1
fi

# Create output directory if different from input
if [[ "$OUTPUT_DIR" != "$INPUT_DIR" ]]; then
    mkdir -p "$OUTPUT_DIR"
fi

# Count files
count=$(find "$INPUT_DIR" -maxdepth 1 -name "*.mp4" -o -name "*.MP4" 2>/dev/null | wc -l)

if [[ $count -eq 0 ]]; then
    echo "No MP4 files found in $INPUT_DIR"
    exit 0
fi

echo "Converting $count MP4 file(s) to MPEG-TS..."
echo "Input:  $INPUT_DIR"
echo "Output: $OUTPUT_DIR"
echo

converted=0
failed=0

for mp4 in "$INPUT_DIR"/*.mp4 "$INPUT_DIR"/*.MP4; do
    [[ -f "$mp4" ]] || continue

    filename=$(basename "$mp4")
    name="${filename%.*}"
    ts_file="$OUTPUT_DIR/${name}.ts"

    echo "Converting: $filename -> ${name}.ts"

    if ffmpeg -y -i "$mp4" -c:v copy -c:a copy -f mpegts "$ts_file" -loglevel warning; then
        echo "  Done: $ts_file"
        ((converted++))
    else
        echo "  Failed: $filename"
        ((failed++))
    fi
done

echo
echo "Conversion complete: $converted succeeded, $failed failed"
