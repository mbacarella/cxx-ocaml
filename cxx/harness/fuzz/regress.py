#!/usr/bin/env python3
"""Run the fuzz regressions: each regress/*.ml through fuzz.py's comparison.

    regress.py [file.ml ...]

A file's first line may be "(* FUZZ: levels=L,... flags=F ... *)" (default:
all levels, no flags).  Prints SAME / DIFF per file; exit 1 on any DIFF.
"""
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fuzz  # noqa: E402


def main():
    files = sys.argv[1:] or sorted(
        os.path.join(HERE, 'regress', f) for f in os.listdir(os.path.join(HERE, 'regress'))
        if f.endswith('.ml'))
    tmp = tempfile.mkdtemp(prefix='fuzz-regress-')
    fuzz.build_cmtdump(tmp)
    bad = 0
    for f in files:
        with open(f) as fh:
            src = fh.read()
        m = re.match(r'\(\* FUZZ: levels=(\S+)(?: flags=([^\n]*))?', src)
        levels = m.group(1).split(',') if m else fuzz.ALL_LEVELS
        flags = m.group(2).split() if m and m.group(2) else []
        wd = os.path.join(tmp, os.path.basename(f))
        os.makedirs(wd)
        shutil.copy(f, os.path.join(wd, 'fz.ml'))
        findings, notes, _ = fuzz.compare_levels(wd, levels, flags)
        if findings:
            bad += 1
            print('DIFF %s: %s' % (os.path.basename(f), ' '.join(sorted({l for l, _, _ in findings}))))
        else:
            print('SAME %s %s' % (os.path.basename(f), ' '.join(notes)))
    shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
