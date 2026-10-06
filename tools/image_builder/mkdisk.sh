#!/bin/sh
# Build a GPT disk image with one FAT32 partition and copy files into it.
#
#   mkdisk.sh <image> <size-MiB> <source-directory> [label]
#
# The partition starts at 1 MiB (LBA 2048), so mtools can reach it as
# "<image>@@1M". One sector per cluster keeps the cluster count above the
# FAT32 minimum even for small images and makes multi-cluster files common.
# Needs sgdisk (gdisk) and mtools.

set -eu

image=$1
size_mib=$2
source=$3
label=${4:-JELLYDATA}

sector=512
start=2048
total=$((size_mib * 1024 * 1024 / sector))
# GPT reserves 33 sectors at the end for the backup header and entries.
sectors=$((total - 34 - start + 1))

rm -f "$image"
truncate -s "${size_mib}M" "$image"
sgdisk --clear --new=1:${start}:$((start + sectors - 1)) --typecode=1:0700 \
       --change-name=1:"$label" "$image" > /dev/null

mformat -i "$image@@1M" -F -c 1 -T "$sectors" -v "$label" ::
if [ -n "$(ls -A "$source")" ]; then
    mcopy -s -i "$image@@1M" "$source"/* ::/
fi
