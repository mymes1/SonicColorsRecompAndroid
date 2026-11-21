#!/usr/bin/env python3
"""Produce fail-fast definitions matching the generated recompiler header."""
import pathlib
import re
import sys

header, dest = map(pathlib.Path, sys.argv[1:3])
text = header.read_text(encoding='utf-8')
names = sorted(set(re.findall(r'^PPC_EXTERN_FUNC\(ovr_([A-Za-z0-9_]+)\);$', text, re.M)))
if not names:
    raise SystemExit('Generated recompiler header has no override declarations')
with dest.open('w', encoding='utf-8') as f:
    f.write('#include "ppc_context.h"\n#include <cstdlib>\n')
    for name in names:
        f.write(f'PPC_FUNC(ovr_{name}) {{ std::abort(); }}\n')
