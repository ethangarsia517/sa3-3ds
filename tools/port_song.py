#!/usr/bin/env python3
"""Rewrites an sa3 song (sound/songs/*.s) for the 3DS port, stdin to stdout.

In m4a song tracks, GOTO and PATT are followed by a 4-byte jump target
(`mPtr label`) at whatever offset the byte stream reaches, so most are
unaligned. The .3dsx format can only relocate aligned pointers, so the port
stores these targets self-relative instead (`target - .`), which the
assembler resolves to a constant with no relocation. The mixer adds the
field's address back (m4a_sound_mixer.c, MP2K_event_goto, under PLATFORM_3DS).
Song headers keep normal pointers.

Same approach as pmd-red-3ds's tools/port_song.py.
"""
import re
import sys

JUMP = re.compile(r'^\s*\.byte\s+(GOTO|PATT)\s*(@.*)?$')
PTR = re.compile(r'^(\s*)mPtr(\s+)(.+?)\s*(@.*)?$')


def main():
    after_jump = False
    out = sys.stdout
    for n, line in enumerate(sys.stdin, 1):
        if after_jump:
            m = PTR.match(line.rstrip('\n'))
            if not m:
                sys.exit(f'port_song.py: line {n}: expected mPtr after GOTO/PATT, got: {line.strip()}')
            out.write(f'{m.group(1)}.int{m.group(2)}({m.group(3)}) - .\n')
            after_jump = False
            continue
        out.write(line)
        after_jump = bool(JUMP.match(line.rstrip('\n')))


if __name__ == '__main__':
    main()
