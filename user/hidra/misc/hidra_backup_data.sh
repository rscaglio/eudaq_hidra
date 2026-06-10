#!/usr/bin/env bash

set -euo pipefail

##############################################################################
# Configuration
##############################################################################

if [[ -z "${EUDAQHIDRA:-}" ]]; then
    echo "ERROR: EUDAQHIDRA environment variable is not set."
    exit 1
fi


LOCAL_DIR_DATA="${EUDAQHIDRA}/run/out_data"
BACKUP_DIR_DATA="/home/eudaq/cernbox/TB2026_H8/raw"
LOCAL_DIR_LOG="${EUDAQHIDRA}/run/logs"
BACKUP_DIR_LOG="/home/eudaq/cernbox/TB2026_H8/logs"

##############################################################################
# Transfer
##############################################################################


echo "============================================================"
echo "Starting local rsync"
echo "Source data      : $LOCAL_DIR_DATA"
echo "Destination data : $BACKUP_DIR_DATA"
echo "Source log       : $LOCAL_DIR_LOG"
echo "Destination log  : $BACKUP_DIR_LOG"
echo "============================================================"

rsync \
    -av \
    --update \
    --partial \
    --human-readable \
    --stats \
    "${LOCAL_DIR_DATA}/" \
    "${BACKUP_DIR_DATA}/"

rsync \
    -av \
    --update \
    --partial \
    --human-readable \
    --stats \
    "${LOCAL_DIR_LOG}/" \
    "${BACKUP_DIR_LOG}/"

echo "============================================================"
echo "Done."
echo "============================================================"



