#!/bin/bash
set -e

BASE=$(cd "$(dirname "$0")/.." && pwd)

export LD_LIBRARY_PATH=$BASE/build/deps/lib

$BASE/build/deps/bin/boss $BASE/Tests/repl-tests.scm