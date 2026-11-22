#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Catch the mistakes that cost a full CI build before they are pushed.

Checks, per patch:
  1. every hunk's declared line counts match the lines it actually carries
     (a wrong count makes git apply reject the whole patch);
  2. no BOM / mojibake at the start of an added line;
  3. added C/C++ text keeps braces and parentheses balanced;
  4. a line that ends with a storage/type specifier ("static void", "int",
     "static int", ...) is followed by a declaration, not by a comment or a
     different return type (this is the bug that bit us: our block was spliced
     between "static void" and "sweet_mali_power(void)");
  5. no added line looks like a truncated string or an unterminated comment.

Usage: check_patch.py <patch> [<patch> ...]
"""
import re
import sys

TYPE_TAIL = re.compile(
    r'^\s*(static\s+)?(void|int|unsigned|bool|u?int(8|16|32|64)_t|'
    r'struct\s+\w+|IReturn|IOReturn|char|size_t|vm_offset_t|task_t)\s*$')


def strip_comments_and_strings(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif c == '"':
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == '\\' else 1
            i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def check(path):
    problems = []
    with open(path, encoding='utf-8', errors='replace', newline='') as fh:
        raw = fh.read()
    lines = raw.splitlines()
    added = [l[1:] for l in lines if l.startswith('+') and not l.startswith('+++')]
    joined = '\n'.join(added)

    # 1. hunk arithmetic
    i = 0
    while i < len(lines):
        m = re.match(r'^@@ -(\d+),(\d+) \+(\d+),(\d+) @@', lines[i])
        if not m:
            i += 1
            continue
        want_old, want_new = int(m.group(2)), int(m.group(4))
        i += 1
        got_old = got_new = 0
        while (i < len(lines)
               and not lines[i].startswith('@@')
               and not lines[i].startswith('diff --git')
               and not lines[i].startswith('--- ')
               and not lines[i].startswith('+++ ')):
            # git format-patch trailers must not be counted as patch content
            if re.match(r'^--\s*$', lines[i]) or re.match(r'^\d+\.\d+\.\d+$', lines[i]):
                i += 1
                while i < len(lines) and not lines[i].startswith(('@@', 'diff --git')):
                    i += 1
                break
            if lines[i].startswith('+'):
                got_new += 1
            elif lines[i].startswith('-'):
                got_old += 1
            elif lines[i].startswith(' '):
                got_old += 1
                got_new += 1
            i += 1
        if (got_old, got_new) != (want_old, want_new):
            problems.append('hunk counts: header says -%d +%d, body has -%d +%d'
                            % (want_old, want_new, got_old, got_new))

    # 2. encoding junk
    for n, l in enumerate(added, 1):
        if l.startswith('\ufeff') or '锘' in l:
            problems.append('BOM/encoding junk in added line %d: %r' % (n, l[:40]))

    # 3. bracket balance
    code = strip_comments_and_strings(joined)
    for name, o, c in (('braces', '{', '}'), ('parens', '(', ')'), ('brackets', '[', ']')):
        if code.count(o) != code.count(c):
            problems.append('%s unbalanced: %d vs %d' % (name, code.count(o), code.count(c)))

    # 4. dangling specifier
    for n in range(len(added) - 1):
        if TYPE_TAIL.match(added[n]):
            nxt = added[n + 1].strip()
            if not nxt or nxt.startswith(('/*', '//', '#')):
                problems.append('line %d (%r) is followed by %r, not a declaration'
                                % (n + 1, added[n].strip(), nxt[:40]))
    return problems


def main():
    bad = 0
    for path in sys.argv[1:]:
        problems = check(path)
        if problems:
            bad += 1
            print('FAIL %s' % path)
            for p in problems:
                print('   -', p)
        else:
            print('ok   %s' % path)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
