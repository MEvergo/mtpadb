#!/system/bin/sh

/system/bin/setprop ctl.stop mtpadb_ksu_adbd 2>/dev/null || true
/system/bin/setprop ctl.stop mtpadb_ksu_rpcd 2>/dev/null || true
