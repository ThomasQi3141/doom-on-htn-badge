#!/bin/sh
# Harness entry point. The badge runs Lua 5.4, so the harness must too:
# Homebrew's linked `lua` is 5.5, with different syntax and for-loop rules.
LUA=/opt/homebrew/opt/lua@5.4/bin/lua
[ -x "$LUA" ] || LUA=$(command -v lua5.4 || echo lua)
cd "$(dirname "$0")" || exit 1

case " $* " in
  *" --no-selftest "*) ;;
  *)
    "$LUA" selftest.lua || {
      echo "  harness selftest failed -- no measurement below can be trusted."
      exit 1
    } ;;
esac
exec "$LUA" run.lua "$@"
