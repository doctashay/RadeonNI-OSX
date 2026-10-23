#!/usr/bin/env python3
"""Find programs that copy glGetString(GL_EXTENSIONS) into a stack buffer, before they run.

  scripts/gl-ext-scan.py <ppc Mach-O under the checkout>   (disassembles with the toolchain container)
  scripts/gl-ext-scan.py -t <otool -tV output>             (a disassembly made elsewhere)

Mesa's extension list is 7.9 KB. Apple's lists on the cards of 2005 were
2 to 3 KB, and some games copy the string into a fixed buffer without
looking at its length (The Sims 2: strcpy into 4096 bytes of its stack
frame, crash at the return address; docs/agents/accel-state.md). The call
sequence is easy to see in PowerPC code:

    li   r3,0x1f03          GL_EXTENSIONS
    bl   _glGetString
    or   r4,r3,r3           the string is the source,
    addi r3,r1,0x210        a place in this function's frame the destination
    bl   <copy>

The scan finds each GL_EXTENSIONS fetch whose first call afterwards has a
stack address in r3 and the string in r4 (or r5, r6 for sprintf-like
callees), and says how much room is left in the frame (from the stwu of the
prologue). A copy that takes a size (strlcpy, snprintf: a size set in r5
or r4 after the string) is reported as bounded. It does not follow the
string through other registers or memory; a program it passes can still
overflow, and a program it flags may truncate correctly.

Exit status: 0 nothing found, 1 something found (unbounded or tight), 2 error.

Copyright (c) 2026 kouta-kun and Claude
SPDX-License-Identifier: MIT
"""
import os
import re
import subprocess
import sys

LIST_BYTES = 8000     # Mesa's GL_EXTENSIONS here, a little rounded up
WINDOW = 16           # instructions looked at after the glGetString call


def parse(text):
    out = []
    for line in text.splitlines():
        m = re.match(r'^([0-9a-f]{8})\t(\S+)(?:\t([^;]*))?(?:\t*; (.*))?$', line)
        if m:
            out.append((int(m.group(1), 16), m.group(2),
                        [a.strip() for a in (m.group(3) or '').strip().split(',')] if m.group(3) else [],
                        m.group(4) or ''))
    return out


def num(s):
    return int(s, 16) if s.startswith(('0x', '-0x')) else int(s)


def frame_size(ins, at):
    """The stwu of the function's prologue, looking back, as the bytes of frame."""
    for i in range(at, max(at - 6000, 0), -1):
        op, args = ins[i][1], ins[i][2]
        if op == 'stwu' and len(args) == 2 and args[0] == 'r1' and args[1].endswith('(r1)'):
            n = num(args[1][:-4])
            if n < 0 or n >= 0x8000:
                return (-n) & 0xffff if n < 0 else 0x10000 - n
            return -n
        if op == 'stwux' and args[:2] == ['r1', 'r1']:
            return None
    return None


def scan(ins):
    found = []
    for i, (addr, op, args, note) in enumerate(ins):
        if op != 'bl' or not note.endswith('_glGetString'):
            continue
        if not any(ins[j][1] == 'li' and ins[j][2] == ['r3', '0x1f03'] for j in range(max(i - 3, 0), i)):
            continue
        alias = {'r3'}           # registers holding the string
        stack = {}               # register -> offset from r1
        sized = False
        for j in range(i + 1, min(i + 1 + WINDOW, len(ins))):
            _, o, a, n = ins[j]
            if o == 'or' and len(a) == 3 and a[1] == a[2]:
                if a[1] in alias:
                    alias.add(a[0])
                else:
                    alias.discard(a[0])
            elif o == 'addi' and len(a) == 3 and a[1] == 'r1':
                stack[a[0]] = num(a[2])
                alias.discard(a[0])
            elif o in ('li', 'addi', 'addis', 'lwz', 'lis', 'mr') and a:
                if a[0] in ('r4', 'r5') and a[0] not in alias:
                    sized = sized or (o == 'li' and a[0] in ('r4', 'r5'))
                alias.discard(a[0])
                stack.pop(a[0], None)
            if o in ('bl', 'bctrl', 'b', 'blr'):
                if o == 'bl' and 'r3' in stack and any(r in alias for r in ('r4', 'r5', 'r6')):
                    fs = frame_size(ins, i)
                    off = stack['r3']
                    room = fs - off if fs is not None else None
                    found.append((addr, off, fs, room, sized))
                break
    return found


def disassemble(path):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.abspath(path)
    if not path.startswith(root + os.sep):
        sys.exit('%s: put the file under the checkout (the container mounts only it)' % path)
    cmd = 'powerpc-apple-darwin8-otool -arch ppc -tV %s 2>/dev/null || powerpc-apple-darwin8-otool -tV %s' % (
        repr(path), repr(path))
    r = subprocess.run([os.path.join(root, 'scripts', 'darwin.sh'), 'sh', '-c', cmd],
                       capture_output=True, text=True)
    if r.returncode or not r.stdout:
        sys.exit('otool failed: ' + r.stderr.strip())
    return r.stdout


def main():
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == '-t':
        text = open(args[1]).read()
    elif len(args) == 1 and not args[0].startswith('-'):
        text = disassemble(args[0])
    else:
        sys.exit(__doc__)
    ins = parse(text)
    if not ins:
        sys.exit('no PowerPC text found')
    sites = scan(ins)
    bad = 0
    for addr, off, fs, room, sized in sites:
        verdict = 'bounded copy' if sized else 'UNBOUNDED copy'
        if room is None:
            tight = '; frame size not found'
        else:
            tight = '; %d bytes of frame after it' % room
        risky = (not sized) and (room is None or room < LIST_BYTES)
        bad += risky
        print('%08x: GL_EXTENSIONS into r1+0x%x (%s%s)%s' % (
            addr, off, verdict, tight, ' <-- overflows a %d-byte list' % LIST_BYTES if risky else ''))
    if not sites:
        print('no GL_EXTENSIONS copy into a stack buffer found')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
