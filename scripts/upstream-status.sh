#!/usr/bin/env bash
# 列出上游（moonlight-stream/moonlight-qt）有、我们还没处理的提交。
#
# 我们的标准同步方式是 **merge upstream/master**（见 docs/upstream-sync.md）：
# merge 进来的提交会自动从 master..upstream/master 范围里消失，所以这个脚本
# 的职责是盯住「还没被 merge 带进来、也没被评估过」的存量。
#
# 判重的三层口径：
#   1. 范围判重 —— master..upstream/master 本身就排除了已 merge 进来的提交；
#   2. 锚点判重 —— 如果走手工摘提交的路子，必须用 `git cherry-pick -x`，
#      提交信息里的「(cherry picked from commit <sha>)」会被这里识别成已合入；
#   3. 疑似重做 —— 我们有时不等上游、直接独立重实现某个功能（历史上 color
#      range 系列就是这么干的）。这类提交靠主题里的版本号 token 做启发式提示，
#      打上「疑似重做」标记，**判断仍归人**：确认覆盖了就记入 upstream-skip.txt。
#
# 用法：
#   scripts/upstream-status.sh              拉取上游后列出
#   scripts/upstream-status.sh --no-fetch   跳过拉取，用本地已有的 upstream/master

set -euo pipefail

UPSTREAM_URL="https://github.com/moonlight-stream/moonlight-qt.git"
BASE_BRANCH="master"
SKIP_FILE="$(dirname "$0")/upstream-skip.txt"

if [ "${1:-}" != "--no-fetch" ]; then
    if ! git remote get-url upstream > /dev/null 2>&1; then
        echo "添加 upstream remote..."
        git remote add upstream "$UPSTREAM_URL"
    fi
    # 防手滑往上游推。放在 if 外面：remote 可能是别人先手工加的，那种情况下
    # push url 还是继承 fetch url，有上游写权限的人一个 git push upstream 就出去了。
    git remote set-url --push upstream DISABLED
    echo "拉取上游..."
    git fetch upstream --quiet
fi

# 手工摘提交留下的锚点（标准是 merge，这条路是备选）
merged=$(git log "$BASE_BRANCH" --format=%B | grep -oE "cherry picked from commit [0-9a-f]{40}" | awk '{print $5}' || true)
# 还在别的分支上（PR 进行中）。只看本地分支和 origin 的远端分支 —— 用 --all 会把
# upstream/* 和 tag 也算进来，上游自己在分支间 cherry-pick 过的提交就会被误判成
# 「我们正在处理」，从待评估里凭空消失。
inflight_refs=$(git for-each-ref --format='%(refname)' refs/heads refs/remotes/origin || true)
if [ -n "$inflight_refs" ]; then
    inflight=$(git log $inflight_refs --not "$BASE_BRANCH" --format=%B | grep -oE "cherry picked from commit [0-9a-f]{40}" | awk '{print $5}' || true)
else
    inflight=""
fi
# 「疑似重做」比对基准：我们自己的提交主题（只需要主题行，量小、够用）
master_subjs=$(git log "$BASE_BRANCH" --format=%s || true)

pending=0
suspect=0
while read -r full short subj; do
    [ -z "$full" ] && continue

    if grep -q "$full" <<< "$merged"; then
        printf '  已合入   %s  %s\n' "$short" "$subj"
        continue
    fi

    if grep -q "$full" <<< "$inflight"; then
        printf '  进行中   %s  %s\n' "$short" "$subj"
        continue
    fi

    # 主动决定不跟的，原因记在 upstream-skip.txt
    if [ -f "$SKIP_FILE" ] && reason=$(grep "^$short" "$SKIP_FILE" | head -1 | cut -d' ' -f2-); [ -n "${reason:-}" ]; then
        printf '  已跳过   %s  %s\n           └─ %s\n' "$short" "$subj" "$reason"
        continue
    fi

    # 疑似重做：上游主题里的版本号 token 在我们的提交主题里也出现过
    hint=""
    for tok in $(printf '%s' "$subj" | grep -oE '[0-9]+(\.[0-9]+)+' | sort -u); do
        if grep -qF -- "$tok" <<< "$master_subjs"; then
            hint="我们的提交里出现过 $tok"
            break
        fi
    done
    if [ -n "$hint" ]; then
        printf '☆ 疑似重做 %s  %s\n           └─ %s，确认覆盖就记入 skip 台账\n' "$short" "$subj" "$hint"
        pending=$((pending + 1))
        suspect=$((suspect + 1))
        continue
    fi

    printf '★ 待评估   %s  %s\n' "$short" "$subj"
    pending=$((pending + 1))
done < <(git log --no-merges --format="%H %h %s" "$BASE_BRANCH..upstream/$BASE_BRANCH")

echo
if [ "$pending" -eq 0 ]; then
    echo "没有待评估的上游提交。"
else
    echo "$pending 个待处理（其中 $suspect 个疑似已被重实现覆盖）。"
    echo "流程见 docs/upstream-sync.md：要跟 → merge upstream/master（手工摘提交必须带 -x）；"
    echo "不跟/已重实现 → 把 <短 sha> 和原因写进 $SKIP_FILE。"
fi
