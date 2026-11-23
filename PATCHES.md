# tdPatch 补丁与验收

## TDLib 1.8.71 / layer 234

完整同步上游 `1b9efe84d6e9a2e2f6eadeeea32b0726d2b8c25d`，包含 Bot 私聊邀请请求和受邀端服务消息。
`test/check_patch_delta.py` 以这次上游提交为基线，逐行核对 14 个生产文件的本地补丁。
限流分类、会话调度、更新过滤与投递、请求响应和网络统计仍由 `test-patches` 回归覆盖。
`Layer234Header` 检查正常和匿名连接头，`ClientFingerprintAndCompression` 另通过真实 Td 初始化检查正常连接头。

原生库的 `getOption("version")` 应返回 `1.8.71`，`getOption("commit_hash")` 应返回本次构建的 tdPatch 完整提交 SHA。
Go 绑定从同一个 tdPatch 提交生成。构建和测试命令见下文的本地验收。

## layer 233 迁移记录

- 旧上游：`d1085f9cebc5a62379991ae1652673954f229c1f`，TDLib 1.8.67 / layer 229。
- 旧补丁基线：`401e262ed`。
- 新上游：`c8828d50f5eac2fc7089ce1089d7ce9095c61b5e`，TDLib 1.8.70 / layer 233。

同步上游协议与配套实现，不另外为下游接入钱包、Widget、社区等业务功能。
原生库和 go-tdlib 必须成套更新，不能只修改 layer 常量或单独替换动态库。

## 补丁迁移

| 行为 | 升级处理 |
|---|---|
| 不追加 application_version 的 TDLib 后缀 | 保留 Td 参数处理补丁 |
| 出站随机 padding | 保留 RawConnection 强制开关及 Transport 的 Desktop 分布 |
| 出站不使用 gzip | 保留 NetQueryCreator 补丁 |
| PFS 恒开、临时密钥 86400 秒、不发送 dropTempAuthKeys | 保留 Dispatcher / Session 补丁 |
| 主会话 30/45/60 秒 ping，媒体会话仅首次 ping | 保留 SessionConnection 补丁 |
| 空闲媒体连接不忙循环；缺失 pong 仍关闭 | 保留 SessionConnection 与 Session 调度保护 |
| 导入缺少 my_id 的会话后重放初始化；关闭期间禁止重放 | 保留 AuthManager 补丁 |
| FLOOD_WAIT 与 SLOWMODE_WAIT 范围不丢失 | 保留 RetryAfter、Global、NetQueryDelayer；错误分类迁至 MessageQueryManager |

旧 `MessagesManager::after_get_difference` 中 Archive 未读数重算及其
`CHECK(list != nullptr)` 已被上游 `6a7cd24b6` 整段删除，并加上授权条件。
对应的本地空指针保护随死代码退役，不复活旧重算路径。
这不替代 AuthManager 的导入初始化与关闭保护。

## TGDirector 原生侧优化

本轮只调整对外更新投递和网络统计初始化，不裁剪 schema、业务 Manager、服务器更新处理或后台刷新。

- `Td::send_update` 在回调投递和 JSON 编码前，丢弃 TGDirector `updateHub` 原有黑名单中的
  22 类更新：文件进度、聊天动作、在线状态、部分贴纸列表、主题配色和动态浏览更新。
  消息、动态发布结果、授权、连接、冻结及已读更新仍正常投递。请求响应走 `send_result`，
  不受更新过滤影响；例如下载请求返回的 `file` 仍保留。
- 初始化时先预设 `disable_network_statistics=true`，再处理预授权 `setOption`，最后决定是否
  创建 `NetStatsManager`。因此无需 TGDirector 新增设置流程，新库与已有库都默认不创建统计 Manager。
  显式排在 `setTdlibParameters` 前的 `setOption(false)` 可以为本次 client 恢复统计；这个覆盖
  不跨 client 生效，下次打开同一数据库仍先预设为 true。初始化后改选项不会动态重建统计 Manager。
- 更新过滤是此 fork 的固定行为，不是通用 TDLib 的完整更新契约。未来下游消费这些更新时，
  必须同步调整原生过滤与行为测试；现有 Go 侧过滤保留，兼容尚未升级的原生库。

`test/patch_profile.cpp` 使用已确认的两个 Seam：`Td::send_update → TdCallback` 和
`Td::request → TdCallback`。测试覆盖全部 22 类过滤、关键更新与请求响应保留、关闭通知，
以及统计请求在初始化前排队、初始化后调用、显式覆盖和持久化 false 后重新打开的行为。
测试不连接 Telegram、不使用真实账号；这些结果不代表真实负载性能数据或 Windows 实机验收。

独立调度器测试必须清理线程本地状态：握手信号量保存线程本地 ActorId，销毁调度器后不能
让下一轮调度器复用它。重开测试等待 TdCallback 销毁后结束调度器，并清理线程本地状态。

本轮 macOS Release 构建通过，新增测试完成红→绿并连续运行 30 轮通过；源码审计通过，
Standards / Spec 独立审查均无发现。全量 CTest 重跑通过（两个目标，约 432 秒）。此前一次完整
运行在上游 `AtomicRead.simple2` 的 `StealingQueue.cpp:98` 失败；仅链接未改动 `tdutils` 的
`test-tdutils -f +AtomicRead_simple2` 单独运行 20 轮也失败 3 次，证明该不稳定测试不依赖本轮
TDLib 改动。本轮不修改该并发实现，也不把一次全量通过当作已修复此问题。

## 本地验收

```sh
python3 test/check_patch_delta.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tdjson test-patches run_all_tests --parallel 3
ctest --test-dir build --output-on-failure
```

`check_patch_delta.py` 要求完整 Git 历史。它对比两次上游基线上的自有补丁增删行，
验证 13 个原位置文件的旧补丁保留，并验证限流补丁迁移及 Archive 旧代码退役。
TGDirector 的两项优化以固定增删行纳入同一审计，不跳过 Td.cpp 或放宽其旧补丁检查。
它是源码差异审计，不是运行时证明。发布工作流在 Windows、macOS 都执行该审计和
`test-patches`，其中行为测试设置 60 秒期限。

`test-patches` 使用合成数据：

- `Layer234Header` 验证正常与匿名连接头声明 234。
- `ClientFingerprintAndCompression`：通过真实 Td 初始化后读取连接头，验证版本字符串不加后缀；
  经 NetQueryCreator 检查多种长度的可压缩请求仍未压缩。初始化前设置 networkTypeNone。
- `DesktopPadding`：检查各 TL 对齐长度的 padding 边界及全部 16 档分布。
- `MainSessionPing`、`MediaSessionIdle`：真实 SessionConnection + 传输边界 fake，
  通过 TL ping/pong 与时间推进验证节奏、唤醒期限及媒体连接空闲关闭。
- `MissingPongClosesMain/Media`：无 pong 时不会因取消周期 ping 而漏关连接。
- `SendErrorKeepsWaitScope`、`RetryAfter_*`：等待预算转换后仍保留范围，进入新版错误处理后仍可解析。
- `TGDirectorUpdateDelivery`：完整过滤黑名单，保留关键更新、请求响应和关闭通知。
- `TGDirectorNetworkStatistics*`：默认禁用统计，允许本次预授权覆盖，重开时重新应用预设。

本次已分别临时恢复版本后缀、打开 gzip、移除媒体唤醒保护、恢复上游 padding、
抹掉等待范围，确认相应用例失败；恢复补丁后重新通过。
这些临时改动不进入交付。

### 仅源码验证项

以下项目由差异审计及独立审查确认补丁保留，本次**没有运行时覆盖**：

- Dispatcher 强制 PFS、临时密钥 86400 秒，以及不发送 dropTempAuthKeys。
  ping 测试用长期合成密钥且关闭 PFS，不能证明密钥交换行为。
- RawConnection 强制启用随机 padding；分布测试直接设置该开关，只覆盖 Transport 算法。
- 外层 `Session::loop` 的唤醒保护；行为测试只驱动 SessionConnection。
- 主会话 30 秒机会式 ping；现有测试覆盖 45 秒强制 ping、60 秒超时及媒体首次 ping。
- 成功联网后的导入初始化重放；关闭期间的事故路径另由下游离线测试覆盖。

独立 Standards 审查未发现问题；Spec 审查未发现具体实现错误，指出以上覆盖缺口。
这些项目不列入“运行时验证通过”，源码保真也不能排除上游交互变化。
Windows/macOS 发布工作流已配置审计及行为测试，但本次没有触发远程 CI。

## 下游配套验证

在 go-tdlib 中以本仓库已提交的 HEAD 生成：

```sh
make generate TD_DIR=/path/to/tdPatch
CGO_CFLAGS="-I/path/to/tdPatch" \
CGO_LDFLAGS="-Wl,-rpath,/path/to/tdPatch/build -L/path/to/tdPatch/build -ltdjson" \
go test ./...
```

绑定仓库的 `TestLinkedTDLibMatchesSchema` 实际调用原生库，核对 1.8.71 与生成时的
commit_hash，防止误测旧库。TGDirector 使用临时 Go workspace 引用本地新绑定进行编译、
TDLib 适配层回归及全量 Go 测试；不把本机路径写入其 go.mod。

TGDirector 现成的 `TestCloseWhileRestoringUnknownMyID` 用合成 auth key 与本机不可达代理，
覆盖关闭时 getUsers 回调触发崩溃的事故路径。成功登录后的初始化重放由源码审计守护，
本次不使用真实账号验证其联网成功路径。

## TDLib 1.8.70-p1 交付记录

本次不发布标签、远程版本或更新下游生产依赖锁定，不覆盖现有安装目录。
构建产物位于 `build/`；下游继续使用旧生产锁定，直到两平台产物与 Go 模块成套发布后切换。
本地 macOS 验证不等于 Windows 实机验证，也不证明服务端无法识别客户端指纹。
