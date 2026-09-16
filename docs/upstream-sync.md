# 上游同步流程(SOP)

> 上游:moonlight-stream/moonlight-qt。本文描述「怎么跟、怎么记、谁兜底」,
> 把同步节奏从个人记忆里解放出来 —— 2026-07-27 之后曾失速 51 天、积压 25 个提交。

## 节奏

- **每两周至少同步一次**,哪怕结论是「全部跳过」。
- 兜底:CI 的 **Upstream sync status** workflow(每周一自动跑)会把积压报到
  tracking issue「上游同步状态跟踪(自动化报告)」。issue 常亮 = 该干活了;
  积压清零会自动关闭,有积压会自动重开。

## 流程

1. `scripts/upstream-status.sh` —— 拉上游并列出待处理提交(★ 待评估 /
   ☆ 疑似重做 / 已跳过)。
2. 逐条评估 ★/☆:
   - **要跟** → 走标准 merge:`git merge upstream/master`。
     手工摘个别提交时**必须 `git cherry-pick -x <sha>`**(-x 不能省,判重靠它)。
   - **不跟 / 已独立重实现** → 把 `<短 sha> 原因` 写进 `scripts/upstream-skip.txt`。
     重实现的注明落点(如「已由 #160 重实现」),半年后回看时不用重新考古。
3. merge 后重跑脚本,清单应清零;没清零说明有漏网。
4. 回归:合并冲突大户见下节,解决后**本地把 tests/ 相关测试跑一遍**再推
   (构建:`qmake && make`;测试清单与跑法见 `tests/README.md`)。

## 判重口径(脚本为什么这么写)

- 范围判重:`master..upstream/master` 自动排除已 merge 进来的提交 —— 这是
  默认路径,不需要任何标记。
- 锚点判重:只有手工 `cherry-pick -x` 的提交会被提交信息锚点识别为「已合入」。
- 疑似重做:我们有时不等上游直接重实现(如 2026-08 上游 color range 系列被
  #151/#160 抢先)。脚本按提交主题里的版本号 token 给 ☆ 提示,**判断归人**:
  确认覆盖了就记 skip 台账,别让它们永远挂在待评估里。

## 冲突热点(merge 时重点照顾)

| 文件 | 原因 |
|---|---|
| `app/streaming/session.cpp` | fork 钩子(overlay/剪贴板/USB/HDR)集中地,历史上 64 个 fork 提交动过它 |
| `app/gui/SettingsView.qml` | fork 已把单体设置页拆到 `app/gui/settings/`,上游改动需要手工搬 |
| `app/languages/*.ts` | 机械冲突,`scripts/update_translate.sh` 重新生成即可 |
| `moonlight-common-c` | 已指向 fork 自己的仓库(qiin2333/moonlight-common-c),同步单独走 |

## 依赖升级的双轨问题

上游会升 Qt/SDL/FFmpeg,我们也在 CI 脚本里钉自己的 REV(build.yml 的 Linux
依赖、setup-deps 的 v-tag)。**同步时必看上游的依赖升级提交**:上游升了而我们
没跟,评估是「跟上」「对齐我们的钉法」还是「skip + 记原因」,不要留悬案。
