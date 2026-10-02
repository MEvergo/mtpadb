#!/system/bin/sh

if [ "${KSU:-}" != "true" ]; then
    abort 'MTPADB requires KernelSU or SukiSU Ultra.'
fi

if [ "${KSU_RUNTIME_MODE:-}" = 'late-load' ]; then
    abort 'MTPADB requires early initrc injection; late-load mode is unsupported.'
fi

for binary in mtpadbd mtprpcd mtpadbctl; do
    if [ ! -x "$MODPATH/bin/$binary" ]; then
        abort "Missing executable: $MODPATH/bin/$binary"
    fi
done

if ! mkdir -p /data/adb/mtpadb; then
    abort 'Unable to create /data/adb/mtpadb.'
fi
if ! chown 0:0 /data/adb/mtpadb || ! chmod 0700 /data/adb/mtpadb; then
    abort 'Unable to secure /data/adb/mtpadb.'
fi

ui_print 'MTPADB installed. Provision device.id and device.key before using RPC.'
