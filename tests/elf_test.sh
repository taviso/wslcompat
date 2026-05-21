#!/bin/bash
#
# Verify the execve polyfill recovers from WSL1's mixed-p_align ENOEXEC.
#
# Part of the wslcompat testsuite
# https://github.com/taviso/wslcompat/
#
set -eu

if [[ ! -x ./sys_badalign ]]; then
    echo "sys_badalign not built"
    exit 1
fi

./sys_badalign

exit 0
