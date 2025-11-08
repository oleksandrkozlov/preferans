#!/usr/bin/env bash

# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2025 Oleksandr Kozlov
#
prefix=$(git rev-parse --show-toplevel)

cd $prefix/tests && py.test \
  --project-dir=$prefix \
  --verbose "$@"
