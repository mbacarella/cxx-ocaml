# Sourced by the harnesses: the commands whose GNU (Linux) and BSD (macOS)
# forms differ, and stand-ins for the bash 4 builtins macOS's /bin/bash (3.2)
# lacks -- so the harnesses run with a stock system's tools on both.
#
#   SELF="$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")"
#   . "$(dirname "$SELF")/portable.sh"
#
# Also mind, writing a harness: under `set -u`, bash < 4.4 calls an empty
# array unbound -- expand one as ${a[@]+"${a[@]}"}; `wc -l` pads its count
# on macOS; `sed -i` takes its suffix attached (-i.bak) to mean the same to
# both seds.

# the C locale: the harnesses handle sources as bytes (macOS's sed, grep
# and tr reject a Latin-1 byte as an "illegal byte sequence" under UTF-8),
# and sort the same on every system
export LC_ALL=C

# deterministic archives: Apple's ar and ranlib record the members' and
# the symbol table's times unless ZERO_AR_DATE is set (GNU ar's default is
# deterministic), and two builds compared byte for byte would differ by them
[ "$(uname -s)" = Darwin ] && export ZERO_AR_DATE=1

# timeout SECS CMD...: coreutils' -- where the system has none, bin/timeout
# (perl), on PATH so that `env`, `xargs` and `sh -c` find it too
if ! command -v timeout >/dev/null 2>&1; then
  PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)/bin:$PATH"
  export PATH
fi

# lines_into NAME < input: the array NAME set to input's lines (mapfile -t);
# lines_append NAME < input appends them
lines_append() {
  local __line
  while IFS= read -r __line || [ -n "$__line" ]; do eval "$1+=(\"\$__line\")"; done
}
lines_into() { eval "$1=()"; lines_append "$1"; }

# ncpus: the online processors (nproc)
ncpus() { getconf _NPROCESSORS_ONLN; }

# vm_limit KB: ulimit -v, where the system has the limit (macOS has none)
vm_limit() { ulimit -v "$1" 2>/dev/null || :; }

# file_size FILE: its size in bytes (stat -c %s)
file_size() { wc -c < "$1" | tr -d ' '; }

# capitalize WORD: String.capitalize_ascii (${WORD^})
capitalize() { printf '%s%s\n' "$(printf '%s' "${1:0:1}" | tr a-z A-Z)" "${1:1}"; }

# pct NUM DEN: 100*NUM/DEN to one decimal (computing it in a "$(awk "...")"
# nested in double quotes is mis-parsed by bash 3.2)
pct() { awk -v n="$1" -v d="$2" 'BEGIN { printf "%.1f", 100 * n / d }'; }
