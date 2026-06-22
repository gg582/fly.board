#!/bin/bash
# clean_thumbnail.sh: A script to clean up generated thumbnails while preserving original uploaded files.

# Path to the uploads directory relative to the repository root
UPLOADS_DIR="public/uploads"
THUMBS_DIR="$UPLOADS_DIR/.thumbs"

echo "=== Thumbnail Cleanup Utility ==="

if [ ! -d "$THUMBS_DIR" ]; then
    echo "Thumbnail directory does not exist: $THUMBS_DIR"
    exit 0
fi

# Count existing thumbnail files
THUMB_COUNT=$(find "$THUMBS_DIR" -type f | wc -l)

if [ "$THUMB_COUNT" -eq 0 ]; then
    echo "No thumbnails found in $THUMBS_DIR."
    exit 0
fi

echo "Found $THUMB_COUNT generated thumbnails in $THUMBS_DIR."
read -p "Are you sure you want to delete all thumbnails? [y/N]: " confirm

if [[ "$confirm" =~ ^[yY](es)?$ ]]; then
    echo "Deleting thumbnails..."
    rm -rf "$THUMBS_DIR"/*
    echo "All thumbnails successfully cleaned up. They will be regenerated on demand."
else
    echo "Cleanup cancelled."
fi
