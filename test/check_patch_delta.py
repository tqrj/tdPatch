#!/usr/bin/env python3
"""核对升级前后的自有补丁增删行；这是源码审计，不替代行为回归。"""

from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OLD_UPSTREAM = "d1085f9cebc5a62379991ae1652673954f229c1f"
OLD_PATCHED = "401e262ed"
UPSTREAM = "1b9efe84d6e9a2e2f6eadeeea32b0726d2b8c25d"


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
    # Windows runner 的重定向输出默认可能是 cp1252，中文审计结果必须使用 UTF-8。
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
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
    # TGDirector 专用出口和初始化预设位于旧 Td 参数补丁之前。
    # 固定已审查的增删行，不放宽旧补丁检查，也不从当前工作区动态生成期望。
    profile_td_delta = r"""+  // TGDirector doesn't consume traffic counters. Preset before manager creation, including on existing databases.
+  // Explicit preauthentication setOption requests below can override this preset for the current client.
+  option_manager_->set_option_boolean("disable_network_statistics", true);
+
-    case td_api::updateAccentColors::ID:
-    case td_api::updateEmojiChatThemes::ID:
-    case td_api::updateFavoriteStickers::ID:
+    // TGDirector discards these updates. Stop before callback delivery and JSON encoding;
+    // server updates and internal state have already been processed. Request results use send_result.
+    case td_api::updateFile::ID:
+    case td_api::updateChatAction::ID:
+    case td_api::updateUserStatus::ID:
+    case td_api::updateChatOnlineMemberCount::ID:
+    case td_api::updateStickerSet::ID:
-    case td_api::updateProfileAccentColors::ID:
+    case td_api::updateTrendingStickerSets::ID:
+    case td_api::updateFavoriteStickers::ID:
+    case td_api::updateChatBackground::ID:
+    case td_api::updateChatTheme::ID:
+    case td_api::updateDefaultBackground::ID:
+    case td_api::updateEmojiChatThemes::ID:
+    case td_api::updateChatAccentColors::ID:
+    case td_api::updateAccentColors::ID:
+    case td_api::updateProfileAccentColors::ID:
+    case td_api::updateStory::ID:
+    case td_api::updateStoryDeleted::ID:
+    case td_api::updateChatActiveStories::ID:
+    case td_api::updateStoryListChatCount::ID:
+    case td_api::updateStoryStealthMode::ID:
+      return;
-    case td_api::updateUserStatus::ID:
-    case td_api::updateTrendingStickerSets::ID: {
-      auto update = static_cast<const td_api::updateTrendingStickerSets *>(object.get());
-      auto sticker_sets = update->sticker_sets_.get();
-      VLOG(td_requests) << "Sending update: updateTrendingStickerSets { " << oneline(to_string(update->sticker_type_))
-                        << ", total_count = " << sticker_sets->total_count_
-                        << ", count = " << sticker_sets->sets_.size() << " }";
-      break;
-    }""".splitlines()
    expected["td/telegram/Td.cpp"] = profile_td_delta + expected["td/telegram/Td.cpp"]
    actual = delta(UPSTREAM)
    failures = []
    for path in sorted(expected.keys() | actual.keys()):
        if expected.get(path) != actual.get(path):
            failures.append(path)
    if failures:
        raise SystemExit("魔改补丁差异发生变化，请逐项复核：\n" + "\n".join(failures))
    print(f"PASS: {len(expected)} 个生产文件的自有补丁增删行与已审查预期一致（含限流迁移及 TGDirector 优化）。")
    print("PASS: MessagesManager 与上游一致，未复活已删除的 Archive 重算。")


if __name__ == "__main__":
    main()
