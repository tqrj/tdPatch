#!/usr/bin/env python3
"""核对升级前后的自有补丁增删行；这是源码审计，不替代行为回归。"""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OLD_UPSTREAM = "d1085f9cebc5a62379991ae1652673954f229c1f"
OLD_PATCHED = "401e262ed"
UPSTREAM = "c8828d50f5eac2fc7089ce1089d7ce9095c61b5e"


def git(*args):
    return subprocess.check_output(
        ["git", "-C", str(ROOT), *args], text=True, encoding="utf-8"
    )


def delta(*revisions):
    changes = {}
    path = None
    for line in git(
        "diff", "--no-ext-diff", "--no-renames", "--unified=0", *revisions, "--", "td"
    ).splitlines():
        if line.startswith("+++ "):
            path = line[6:] if line.startswith("+++ b/") else path
        elif line.startswith("--- a/"):
            path = line[6:]
        elif line.startswith(("+", "-")):
            changes.setdefault(path, []).append(line)
    return changes


def main():
    expected = delta(OLD_UPSTREAM, OLD_PATCHED)
    old_messages = expected.pop("td/telegram/MessagesManager.cpp")
    # Archive 重算已由上游 6a7cd24b6 整段删除，不能复活旧代码来保留空指针保护。
    # 错误分类函数搬到了 MessageQueryManager；保留原来的 wait 范围识别。
    moved = [
        '-      if (!begins_with(error_message, "Too Many Requests: retry after ")) {',
        '+      if (!begins_with(error_message, "Too Many Requests: retry after ") &&',
        '+          Global::get_retry_after(error_code, error_message) == 0) {',
    ]
    if old_messages[-3:] != moved:
        raise SystemExit("旧限流补丁基线不匹配")
    expected["td/telegram/MessageQueryManager.cpp"] = moved
    actual = delta(UPSTREAM)
    failures = []
    for path in sorted(expected.keys() | actual.keys()):
        if expected.get(path) != actual.get(path):
            failures.append(path)
    if failures:
        raise SystemExit("魔改补丁差异发生变化，请逐项复核：\n" + "\n".join(failures))
    print(f"PASS: {len(expected)} 个生产文件的自有补丁增删行与基线一致（含限流迁移）。")
    print("PASS: MessagesManager 与上游一致，未复活已删除的 Archive 重算。")


if __name__ == "__main__":
    main()
