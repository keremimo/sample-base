# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Kerem Kilic (Ellic Studio)
# The base and its bundled apps: every source under src/, at any depth up to
# src/a/b/. The build itself is in base.mk, which an app in its own
# repository includes too.
APP_SOURCES := $(wildcard src/*.cpp src/*/*.cpp src/*/*/*.cpp)
include base.mk
