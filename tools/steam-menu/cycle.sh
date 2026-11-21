#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# cycle.sh <serial> [--args "..."] [--env K=V ...] [--wait N] [--dry-run]
# One Steam-menu iteration on a phone. TOUCHES A PHONE: needs the owner's phone free. See README.md.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
exec python3 -I "$here/cycle.py" "$@"
