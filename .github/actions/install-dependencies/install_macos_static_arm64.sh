#!/usr/bin/env bash

# The native macOS dependency installation steps are architecture-independent.
# set_environment_variables.sh selects the matching arm64-osx vcpkg bundle.
exec "$( dirname "$0" )/install_macos_static_x86_64.sh"
