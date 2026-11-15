# layer 233 升级与魔改补丁验收

## 版本与范围

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

## 本地验收

```sh
python3 test/check_patch_delta.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tdjson test-patches run_all_tests --parallel 3
ctest --test-dir build --output-on-failure
```

`check_patch_delta.py` 要求完整 Git 历史。它对比两次上游基线上的自有补丁增删行，
验证 13 个原位置文件完全保留，并验证限流补丁迁移及 Archive 旧代码退役。
它是源码差异审计，不是运行时证明。发布工作流在 Windows、macOS 都执行该审计和
`test-patches`，其中行为测试设置 60 秒期限。

`test-patches` 使用合成数据、不登录真实账号：

- `Layer233Header`：正常与匿名连接头声明 233。
- `ClientFingerprintAndCompression`：通过真实 Td 初始化后读取连接头，验证版本字符串不加后缀；
  经 NetQueryCreator 检查多种长度的可压缩请求仍未压缩。初始化前设置 networkTypeNone。
- `DesktopPadding`：检查各 TL 对齐长度的 padding 边界及全部 16 档分布。
- `MainSessionPing`、`MediaSessionIdle`：真实 SessionConnection + 传输边界 fake，
  通过 TL ping/pong 与时间推进验证节奏、唤醒期限及媒体连接空闲关闭。
- `MissingPongClosesMain/Media`：无 pong 时不会因取消周期 ping 而漏关连接。
- `SendErrorKeepsWaitScope`、`RetryAfter_*`：等待预算转换后仍保留范围，进入新版错误处理后仍可解析。

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

绑定仓库的 `TestLinkedTDLibMatchesSchema` 实际调用原生库，核对 1.8.70 与生成时的
commit_hash，防止误测旧库。TGDirector 使用临时 Go workspace 引用本地新绑定进行编译、
TDLib 适配层回归及全量 Go 测试；不把本机路径写入其 go.mod。

TGDirector 现成的 `TestCloseWhileRestoringUnknownMyID` 用合成 auth key 与本机不可达代理，
覆盖关闭时 getUsers 回调触发崩溃的事故路径。成功登录后的初始化重放由源码审计守护，
本次不使用真实账号验证其联网成功路径。

## 交付边界

本次不发布标签、远程版本或更新下游生产依赖锁定，不覆盖现有安装目录。
构建产物位于 `build/`；下游继续使用旧生产锁定，直到两平台产物与 Go 模块成套发布后切换。
本地 macOS 验证不等于 Windows 实机验证，也不证明服务端无法识别客户端指纹。
