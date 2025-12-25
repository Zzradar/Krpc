#!/usr/bin/env bash
set -euo pipefail

# 在两个 tmux pane 中分别启动 server 与 pool_demo。
# 依赖：tmux（如未安装则打印手工步骤）。

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONF_FILE="${CONF_FILE:-${ROOT_DIR}/bin/test.conf}"
SESSION_NAME="${SESSION_NAME:-krpc-pool-demo}"
# 可选：通过 DEMO_ENV 传入环境变量，DEMO_CMD 覆盖默认 demo 命令
DEMO_ENV="${DEMO_ENV:-}"
DEMO_CMD="${DEMO_CMD:-POOL_DEMO_MODE=new_channel ./bin/pool_demo -i \"${CONF_FILE}\"}"

if command -v tmux >/dev/null 2>&1; then
  tmux kill-session -t "${SESSION_NAME}" 2>/dev/null || true
  tmux new-session -d -s "${SESSION_NAME}" "cd \"${ROOT_DIR}\" && bin/server -i \"${CONF_FILE}\""
  tmux split-window -h -t "${SESSION_NAME}" "cd \"${ROOT_DIR}\" && ${DEMO_ENV} ${DEMO_CMD}"
  tmux select-layout -t "${SESSION_NAME}" tiled
  tmux attach -t "${SESSION_NAME}"
else
  echo "tmux 未安装，请在两个终端手工执行："
  echo "  1) cd \"${ROOT_DIR}\" && bin/server -i \"${CONF_FILE}\""
  echo "  2) cd \"${ROOT_DIR}\" && ${DEMO_ENV} ${DEMO_CMD}"
fi
