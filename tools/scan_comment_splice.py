# -*- coding: utf-8 -*-
"""scan_comment_splice.py -- 扫描「// 注释以反斜杠结尾」导致的行拼接吞行事故。

背景 / Why:
  C/C++ 翻译阶段 2（行拼接）发生在阶段 3（注释识别）**之前**。因此
      // path relative to data\\logic\\
          { "landscapetypes.ini", 0x4162E5, 0x511534 },
  会被拼成一行 —— 下一行源码被整行注释掉，编译**不报错**（仅 MSVC C4010 警告），
  数组静默少一项。本项目曾因此让 kLogicFiles 从 12 项变 11 项，
  循环写死 12 -> 越界读到相邻 .rdata -> call 1 -> EIP=1 崩溃。

  中文注释很容易踩这个坑：`data\\logic\\`、`Data\\maps\\` 这类 Windows 路径
  写在注释末尾时，结尾那个反斜杠就是地雷。

用法 / usage:
  python scan_comment_splice.py <目录> [<目录> ...] [--fix]
    默认只报告；--fix 会删掉行尾那个反斜杠（仅当该行是纯 // 注释行时才自动修）。
"""
import sys, os, argparse

EXTS = ('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.inl')


def find_line_comment(line):
    """返回 // 的起始下标；跳过字符串/字符字面量里的 //。找不到返回 -1。"""
    i, n = 0, len(line)
    in_s = in_c = False
    while i < n:
        ch = line[i]
        if in_s:
            if ch == '\\':
                i += 2
                continue
            if ch == '"':
                in_s = False
        elif in_c:
            if ch == '\\':
                i += 2
                continue
            if ch == "'":
                in_c = False
        else:
            if ch == '"':
                in_s = True
            elif ch == "'":
                in_c = True
            elif ch == '/' and i + 1 < n and line[i + 1] == '/':
                return i
            elif ch == '/' and i + 1 < n and line[i + 1] == '*':
                # 块注释：本扫描器只关心行注释，遇到块注释起点就放弃该行
                return -1
        i += 1
    return -1


def scan_file(path):
    with open(path, 'rb') as f:
        raw = f.read()
    try:
        text = raw.decode('utf-8')
        enc = 'utf-8'
    except UnicodeDecodeError:
        text = raw.decode('gbk', 'replace')
        enc = 'gbk'
    lines = text.split('\n')
    hits = []
    for idx, line in enumerate(lines):
        body = line.rstrip('\r')
        if not body.endswith('\\'):
            continue
        c = find_line_comment(body)
        if c < 0:
            continue          # 行尾反斜杠不在注释里（宏续行等），正常
        swallowed = lines[idx + 1].rstrip('\r') if idx + 1 < len(lines) else '<EOF>'
        pure = body.lstrip().startswith('//')
        hits.append(dict(lineno=idx + 1, text=body, swallowed=swallowed,
                         pure=pure, col=c))
    return hits, lines, enc, ('\r\n' if '\r\n' in text else '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--fix', action='store_true', help='删除行尾反斜杠（仅纯注释行）')
    a = ap.parse_args()

    total = fixed = 0
    for d in a.dirs:
        for root, _dirs, files in os.walk(d):
            if any(p in root for p in ('.git', 'Release', 'Debug', '.temp', 'node_modules')):
                continue
            for fn in sorted(files):
                if not fn.lower().endswith(EXTS):
                    continue
                path = os.path.join(root, fn)
                hits, lines, enc, nl = scan_file(path)
                if not hits:
                    continue
                print('=' * 78)
                print(path)
                for h in hits:
                    total += 1
                    kind = '纯注释行 / pure comment' if h['pure'] else '尾随注释 / trailing comment'
                    print('  L%-5d [%s]' % (h['lineno'], kind))
                    print('    注释: %s' % h['text'])
                    print('    被吞: %s' % h['swallowed'])
                if a.fix:
                    changed = False
                    for h in hits:
                        if not h['pure']:
                            print('    !! L%d 是尾随注释，未自动修改（需人工判断）' % h['lineno'])
                            continue
                        i = h['lineno'] - 1
                        body = lines[i].rstrip('\r')
                        lines[i] = body[:-1].rstrip()
                        changed = True
                        fixed += 1
                        print('    -> 已修 L%d' % h['lineno'])
                    if changed:
                        with open(path, 'w', encoding=enc, newline='') as f:
                            f.write(nl.join(l.rstrip('\r') for l in lines))
    print()
    print('合计命中 %d 处，已修 %d 处。' % (total, fixed))
    if total and not a.fix:
        print('加 --fix 可自动删除纯注释行的行尾反斜杠。')


if __name__ == '__main__':
    main()
