#!/system/bin/sh

identity_dir=/data/adb/mtpadb
if [ -s "$identity_dir/device.id" ] && [ -s "$identity_dir/device.key" ]; then
    /system/bin/setprop ctl.start mtpadb_ksu_rpcd
else
    /system/bin/log -t mtpadb -p w 'RPC daemon not started: provision device.id and device.key first.'
fi
