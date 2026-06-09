#!/usr/bin/env bash

set -euo pipefail

##############################################################################
# Configuration
##############################################################################

if [[ -z "${EUDAQHIDRA:-}" ]]; then
    echo "ERROR: EUDAQHIDRA environment variable is not set."
    exit 1
fi


LOCAL_DIR="${EUDAQHIDRA}/run/out_data"
BACKUP_DIR="/home/eudaq/cernbox/TB2026_H8/raw"

##############################################################################
# Transfer
##############################################################################


echo "============================================================"
echo "Starting local rsync"
echo "Source      : $LOCAL_DIR"
echo "Destination : $BACKUP_DIR"
echo "============================================================"

rsync \
    -av \
    --update \
    --partial \
    --human-readable \
    --stats \
    "${LOCAL_DIR}/" \
    "${BACKUP_DIR}/"

echo "============================================================"
echo "Done."
echo "============================================================"



