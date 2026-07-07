#!/usr/bin/env bash
# Fast all-gates check vs saved baselines in /tmp/base_*.  Prints PASS/FAIL per
# gate as SETS (not totals) plus the accept delta (the metric that should shrink).
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
J="${JOBS:-24}"

JOBS=$J bash cxx/harness/reject_parity.sh >/dev/null 2>&1;    sort /tmp/.reject_files > /tmp/cur_reject.txt
JOBS=$J bash cxx/harness/accept_parity.sh >/dev/null 2>&1;    sort /tmp/.accept_files > /tmp/cur_accept.txt
JOBS=$J bash cxx/harness/sig_parity.sh   >/dev/null 2>&1;     sig=$(grep -c '^MATCH' /tmp/.sig_results); sigd=$(grep -c '^DIFF' /tmp/.sig_results)
JOBS=$J bash cxx/harness/typedtree_parity.sh >/tmp/.tt_out 2>/dev/null; tt=$(grep 'over oracle' /tmp/.tt_out)
JOBS=$J bash cxx/harness/lambda_parity.sh >/dev/null 2>&1;    sort /tmp/.lambda_diff_files > /tmp/cur_lambda_diff.txt

echo "--- reject (must equal baseline set) ---"
if diff -q /tmp/base_reject.txt /tmp/cur_reject.txt >/dev/null; then echo "PASS ($(wc -l </tmp/cur_reject.txt))"; else echo "FAIL"; echo "NEW false-rejects:"; comm -13 /tmp/base_reject.txt /tmp/cur_reject.txt; echo "gone:"; comm -23 /tmp/base_reject.txt /tmp/cur_reject.txt; fi

echo "--- accept (baseline 38; shrinking = win, growth = FAIL) ---"
b=$(wc -l </tmp/base_accept_sorted.txt); c=$(wc -l </tmp/cur_accept.txt)
echo "baseline=$b current=$c"
new=$(comm -13 /tmp/base_accept_sorted.txt /tmp/cur_accept.txt)
[ -n "$new" ] && { echo "NEW false-accepts (FAIL):"; echo "$new"; }
fixed=$(comm -23 /tmp/base_accept_sorted.txt /tmp/cur_accept.txt)
[ -n "$fixed" ] && { echo "FIXED (shrunk):"; echo "$fixed"; }

echo "--- sig (baseline MATCH 688 / DIFF 4) ---"; echo "MATCH=$sig DIFF=$sigd"
echo "--- typedtree (baseline 100.0% over 1016: identical 1016, DIFF 0, err 0) ---"; echo "$tt"
echo "--- lambda DIFF set (baseline 325) ---"
if diff -q /tmp/base_lambda_diff.txt /tmp/cur_lambda_diff.txt >/dev/null; then echo "PASS ($(wc -l </tmp/cur_lambda_diff.txt))"; else echo "CHANGED"; echo "new-diff:"; comm -13 /tmp/base_lambda_diff.txt /tmp/cur_lambda_diff.txt; echo "gone-diff:"; comm -23 /tmp/base_lambda_diff.txt /tmp/cur_lambda_diff.txt; fi
