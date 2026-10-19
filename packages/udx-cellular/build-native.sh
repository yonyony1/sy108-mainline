#!/bin/sh
# Compile the maintained cellular service, typed clients.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=${1:?output directory}
JSON_LIBRARY=${2:?OpenWrt libjson-c library or linker argument}
CC=${CC:-musl-gcc}
CFLAGS=${CFLAGS:--Os -Wall -Wextra -Werror -Wno-misleading-indentation}
SHA=${UDX_SHA_DIR:-$HERE/../../components/cpnv/runtime/components/common}
JSON_INCLUDE=${JSON_INCLUDE:-/usr/include}
SOURCE=${UDX_CELLULAR_SOURCE:-$HERE/src/native}
mkdir -p "$OUT/.cellular-objects"
OBJECTS=$OUT/.cellular-objects
for name in common digest format sms_pdu owner transport data receipts sms_history apn identity operators radio advanced modes info debug telemetry controller service client quiesce; do
    "$CC" ${CPPFLAGS:-} $CFLAGS -std=c11 -I"$JSON_INCLUDE" -I"$SOURCE" -I"$SHA/include" -c "$SOURCE/$name.c" -o "$OBJECTS/$name.o"
done
"$CC" ${CPPFLAGS:-} $CFLAGS -std=c11 -I"$SHA/include" -c "$SHA/src/sha256.c" -o "$OBJECTS/sha256.o"
service_objects=''
for name in common digest format sms_pdu owner transport data receipts sms_history apn identity operators radio advanced modes info debug telemetry controller service sha256; do
    service_objects="$service_objects $OBJECTS/$name.o"
done
"$CC" $CFLAGS $service_objects ${LDFLAGS:-} "$JSON_LIBRARY" -pthread -o "$OUT/udx-cellulard"
for spec in 'client:udx-cellular-control' 'quiesce:udx-system-quiesce'; do
    source=${spec%%:*}
    name=${spec#*:}
    extra=''
    [ "$source" != quiesce ] || extra="$OBJECTS/owner.o"
    "$CC" $CFLAGS "$OBJECTS/$source.o" "$OBJECTS/common.o" $extra ${LDFLAGS:-} "$JSON_LIBRARY" -pthread -o "$OUT/$name"
done
