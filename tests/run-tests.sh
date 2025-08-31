#!/usr/bin/env bash
prefix=$(git rev-parse --show-toplevel)

cd $prefix/tests && py.test \
  --project-dir=$prefix \
  --verbose "$@"
