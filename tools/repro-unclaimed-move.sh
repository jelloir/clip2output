#!/bin/bash
# Reproduce (or confirm the absence of) the unclaimed-window input hole.
#
# An unclaimed window - anything no client has claimed - moved to another screen by any means
# other than an interactive drag used to keep the owner the effect had guessed for it at first
# paint. It was then painted and hit-tested only on the screen it LEFT: invisible where it now
# is, and unclickable there too. A modal dialog in that state locks its whole application, which
# is what made Spectacle's save dialog look like a dead screen.
#
# Usage:  repro-unclaimed-move.sh [resourceClass]      (default: org.kde.spectacle)
#
# Run it against the session you want to test. Needs two screens and the effect loaded.
set -euo pipefail

CLASS="${1:-org.kde.spectacle}"
SCRIPT_FILE=$(mktemp /tmp/clip2output-repro-XXXXXX.js)
trap 'rm -f "$SCRIPT_FILE"' EXIT

if [ "$(qdbus6 org.kde.KWin /Effects org.kde.kwin.Effects.isEffectLoaded clip2output)" != "true" ]; then
    echo "clip2output is not loaded; nothing to test." >&2
    exit 1
fi
echo "effect: $(qdbus6 org.kde.KWin /ClipToOutput inputClippingStatus)"

cat > "$SCRIPT_FILE" <<EOF
var target = workspace.windowList().find(w => w.resourceClass == "$CLASS");
if (!target) {
    print("CLIP2OUTPUT-REPRO no window of class $CLASS is open");
} else if (workspace.screens.length < 2) {
    print("CLIP2OUTPUT-REPRO needs two screens");
} else {
    workspace.activeWindow = target;
    print("CLIP2OUTPUT-REPRO id=" + target.internalId);
    print("CLIP2OUTPUT-REPRO before: output=" + target.output.name);
    workspace.slotWindowToNextScreen();   // the same path as the "Window to Next Screen" shortcut
    print("CLIP2OUTPUT-REPRO after:  output=" + target.output.name);
    // windowAt() goes through the very hitTest slot the effect hooks, so this is the real
    // answer to "would a click here land on the window", with no pointer involved.
    var c = { x: target.frameGeometry.x + target.frameGeometry.width / 2,
              y: target.frameGeometry.y + target.frameGeometry.height / 2 };
    var hit = workspace.windowAt(c);
    print("CLIP2OUTPUT-REPRO hitTest at its real centre: " +
          (hit.length ? "OK, " + hit.map(w => w.resourceClass).join(",") + " (fixed build)"
                      : "NOTHING - INPUT HOLE (old build: the bug is present)"));
}
EOF

id=$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$SCRIPT_FILE")
qdbus6 org.kde.KWin /Scripting/Script$id org.kde.kwin.Script.run
sleep 1
echo "--- result (also in the KWin log) ---"
journalctl --user -u plasma-kwin_wayland -n 200 --no-pager 2>/dev/null | grep -a CLIP2OUTPUT-REPRO | tail -5 \
  || grep -a CLIP2OUTPUT-REPRO /tmp/virtual-kwin-kwin.log | tail -5
