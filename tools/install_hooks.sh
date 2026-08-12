#!/usr/bin/env bash
# Install the local pre-commit gate. Git hooks aren't tracked by the repo, so
# every clone has to run this once:
#
#   Tools/install_hooks.sh
#
# The hook runs the same two gates CI does (design + invariants). CI remains
# the authority -- this just catches violations before you push.
set -euo pipefail
cd "$(dirname "$0")/.."
HOOK=".git/hooks/pre-commit"
cat > "$HOOK" <<'HOOKEOF'
#!/usr/bin/env bash
set -euo pipefail
repo="$(git rev-parse --show-toplevel)"
"$repo/Tools/design_audit.sh"     || { echo "pre-commit: design audit FAILED";     exit 1; }
"$repo/Tools/invariants_audit.sh" || { echo "pre-commit: invariants audit FAILED"; exit 1; }
HOOKEOF
chmod +x "$HOOK"
echo "installed $HOOK (design + invariants gates)"
