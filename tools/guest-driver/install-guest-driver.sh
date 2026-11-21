#!/bin/sh
# Install the Mali-open Vulkan ICD inside the DroidDeck Steam guest rootfs so that the
# guest loads it no matter which driver the app UI/switch has selected.
#
# Run INSIDE the guest session (proot), e.g.
#   droiddeckctl guest -- sh /storage/emulated/0/Download/mali-open-guest/install-guest-driver.sh
# Payload (pushed by the host beforehand, see guest-driver.md):
#   $SRC/libvulkan_panfrost.so   from mali-open-<variant>.zip
#   $SRC/vkprobe.py              vkEnumeratePhysicalDevices prober (optional but used for the check)
#
# What it does (idempotent, reversible):
#   1. .so        -> /usr/local/lib/mali-open/libvulkan_panfrost.so   (glibc+wayland build)
#   2. manifest   -> /usr/share/vulkan/icd.d/mali-open_icd.json       (absolute library_path)
#   3. every other icd.d/*.json  -> /usr/share/vulkan/icd.d.disabled/ (loader has exactly one)
#   4. vkbridge/droidbridge_icd.json -> *.disabled (the app recreates it each session, see 5)
#   5. session hook: /usr/local/bin/droiddeck-session sources the drop-in
#      /usr/local/lib/mali-open/mali-open.env just before it starts the session, so
#      VK_ICD_FILENAMES/VK_DRIVER_FILES point at our manifest even when the app set them to
#      its bridge/PanVK manifest (pref vkBridge=true, linuxDriver=<bundle>).
#      The app re-stages /usr/local/* from the APK whenever SessionFiles' StageLedger
#      (/usr/local/.droiddeck-staged: "path<TAB>length mtime canExecute") does not match the
#      file, so after patching we rewrite that one ledger line; the patch then survives
#      session restarts.
#
# Rollback: run this script with --uninstall. Deleting only the drop-in is NOT enough: step 3
# moved every other icd.d manifest into icd.d.disabled, so the loader would still find ours
# (the app's bridge off path picks the first *.json in icd.d). --uninstall removes our manifest
# and the drop-in and moves the displaced icd.d and bridge manifests back. To drop the session
# hook as well, replace /usr/local/bin/droiddeck-session with droiddeck-session.orig-mali-open
# (step 5 keeps it) or touch the script so the app re-stages its own copy.
set -eu

SRC=${1:-/storage/emulated/0/Download/mali-open-guest}
DEST=/usr/local/lib/mali-open
ICD_DIR=/usr/share/vulkan/icd.d
ICD=$ICD_DIR/mali-open_icd.json
ENVF=$DEST/mali-open.env
MARK=mali-open.env

say() { printf '%s\n' "$*"; }

# Rollback. Kept in this script so the paths that step 3/4 moved are restored by the same code
# that moved them; "first manifest in icd.d wins" means leaving icd.d.disabled populated would
# still select ours.
if [ "${1:-}" = "--uninstall" ]; then
   rm -f "$ICD" "$ENVF"
   say "removed $ICD and $ENVF"
   for f in "$ICD_DIR.disabled"/*.json; do
      [ -e "$f" ] || continue
      mv -f "$f" "$ICD_DIR/"
      say "restored manifest: $f"
   done
   for f in /usr/local/share/vkbridge/*_icd.json.disabled; do
      [ -e "$f" ] || continue
      mv -f "$f" "${f%.disabled}"
      say "restored bridge manifest: $f"
   done
   say "rollback ok: the app's own driver selection is back at the next session start"
   say "  (the session hook stays inert without the drop-in; droiddeck-session.orig-mali-open has the original)"
   exit 0
fi

[ -r "$SRC/libvulkan_panfrost.so" ] || { say "payload missing: $SRC/libvulkan_panfrost.so"; exit 2; }
mkdir -p "$DEST" /usr/local/bin "$ICD_DIR" "$ICD_DIR.disabled" /usr/local/share/vkbridge

# 1. driver library
install -m 0755 "$SRC/libvulkan_panfrost.so" "$DEST/libvulkan_panfrost.so"
[ -r "$SRC/vkprobe.py" ] && install -m 0755 "$SRC/vkprobe.py" /usr/local/bin/mali-open-vkprobe
cat > "$DEST/VERSION" <<EOF
$(sed -n 1p "$SRC/VERSION" 2>/dev/null || echo unknown)
sha256 $(sha256sum "$DEST/libvulkan_panfrost.so" | cut -d' ' -f1)
EOF

# 2. manifest with an absolute library_path (the loader resolves relative paths against icd.d)
cat > "$ICD" <<EOF
{"file_format_version":"1.0.1","ICD":{"library_path":"$DEST/libvulkan_panfrost.so","api_version":"1.3.0"}}
EOF
chmod 0644 "$ICD"

# 3. one manifest only (the app picks the first *.json in this dir for VK_ICD_FILENAMES)
for f in "$ICD_DIR"/*.json; do
  [ -e "$f" ] || continue
  [ "$f" = "$ICD" ] && continue
  mv -f "$f" "$ICD_DIR.disabled/"
  say "disabled manifest: $f"
done

# 4. the bridge manifest (recreated by the app on every session start, the hook below wins)
for f in /usr/local/share/vkbridge/*_icd.json; do
  [ -e "$f" ] || continue
  mv -f "$f" "$f.disabled"
  say "disabled bridge manifest: $f"
done

# 4b. undo experiments: the vendor bridge client library must stay the vendor's
L=/usr/local/lib/vkbridge/libvulkan_droidbridge.so
if [ -L "$L" ] && [ -f "$L.vkbridge-orig" ]; then rm -f "$L"; mv "$L.vkbridge-orig" "$L"; say "restored $L"; fi
rm -f /usr/local/bin/zz-canary.txt /usr/local/lib/vkbridge/zz-canary.txt /usr/local/share/vkbridge/zz-canary.txt

# 5. session hook + drop-in
cat > "$ENVF" <<EOF
# mali-open guest driver (tools/guest-driver/install-guest-driver.sh)
export VK_ICD_FILENAMES=$ICD
export VK_DRIVER_FILES=$ICD
export BL_VKBRIDGE=0
export BL_OPEN_DRIVER=1
EOF
chmod 0644 "$ENVF"

SESS=/usr/local/bin/droiddeck-session
[ -f "$SESS" ] || { say "no $SESS - session hook skipped"; exit 0; }
if grep -q "$MARK" "$SESS"; then
  say "session hook: already present"
else
  cp -p "$SESS" "$SESS.orig-mali-open"
  python3 - "$SESS" <<'PY'
import sys
path = sys.argv[1]
s = open(path).read()
anchor = 'if [ "${BL_GAME_LOGS:-0}" = 1 ]'
assert s.count(anchor) == 1, "anchor not unique: %d" % s.count(anchor)
hook = ('# mali-open guest driver: force our ICD into the session env (drop-in, delete to revert)\n'
        'if [ -r /usr/local/lib/mali-open/mali-open.env ]; then\n'
        '  . /usr/local/lib/mali-open/mali-open.env\n'
        '  echo "== vulkan driver: mali-open ICD ($VK_ICD_FILENAMES; app bridge/pref selection ignored)"\n'
        'fi\n')
open(path, "w").write(s.replace(anchor, hook + anchor, 1))
PY
  chmod 0755 "$SESS"
  say "session hook: installed in $SESS (original kept as $SESS.orig-mali-open)"
fi

# 6. keep the hook: rewrite the app's stage ledger line for the patched script, else the
#    next session start re-stages the APK copy over it.
LEDGER=/usr/local/.droiddeck-staged
if [ -f "$LEDGER" ]; then
  REL=usr/local/bin/droiddeck-session
  python3 - "$LEDGER" "$REL" "$SESS" <<'PY'
import os, sys
path, rel, sess = sys.argv[1], sys.argv[2], sys.argv[3]
st = os.stat(sess)
# Java: length, File.lastModified() (ms), canExecute()
mark = "%d %d %s" % (st.st_size, st.st_mtime_ns // 1000000, "true" if st.st_mode & 0o111 else "false")
lines = open(path).read().split("\n")
out, hit = [], False
for line in lines:
    if line.startswith(rel + "\t"):
        line = rel + "\t" + mark
        hit = True
    out.append(line)
open(path, "w").write("\n".join(out))
print("ledger: %s -> %s (%s)" % (rel, mark, "updated" if hit else "NOT FOUND"))
PY
fi

say "== icd.d"; ls -l "$ICD_DIR"
say "== sha256"; sha256sum "$DEST/libvulkan_panfrost.so"
say "install ok: restart the session, then run:"
say "  VK_LOADER_DEBUG=error,warn,driver python3 /usr/local/bin/mali-open-vkprobe"
