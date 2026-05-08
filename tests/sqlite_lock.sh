#!/bin/bash
#
# Check that sqlite EXCLUSIVE mode actually works.
#
# Part of the wslcompat testsuite
# https://github.com/taviso/wslcompat/
#
set -eu

declare tmpfile=$(mktemp --dry-run --suffix=.db)
declare pipe=$(mktemp --dry-run)

mkfifo --mode=0600 "${pipe}"

exec {pipefd}<>${pipe}

# Cleanup on exit
trap "rm -f '${tmpfile}' '${pipe}'" EXIT

if ! type sqlite3; then
    echo "skipping sqlite test as no binary installed"
    exit 0
fi

# Begin an exclusive session.
sqlite3 "${tmpfile}"                    \
        "BEGIN EXCLUSIVE"               \
        ".shell echo done >&${pipefd}"  \
        ".shell read wait <&${pipefd}"  &

# Wait for that to initialize.
read -u ${pipefd}

echo "The next sqlite should cause an error..."

# Another user tries to edit the same database...
if sqlite3 "${tmpfile}" "BEGIN EXCLUSIVE"; then
    echo "ERROR: multiple sqlite readers permitted"
    exit 1
fi

# The first writer completes
echo "done" >&${pipefd}

wait

# Make sure we can access it now.
if ! sqlite3 "${tmpfile}" "BEGIN EXCLUSIVE"; then
    echo "ERROR: expected exclusive access permitted"
    exit 1
fi

exit 0
