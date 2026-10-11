#!/usr/bin/env bash
# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

set -euo pipefail

build_dir="$(cd "${1:?build directory required}" && pwd)"
build_type="${2:-Release}"
if [[ ! -f "${build_dir}/CTestTestfile.cmake" ]]; then
	echo "Tests were not configured in ${build_dir}. Configure with -Dtests=ON." >&2
	exit 1
fi

mkdir -p "${build_dir}/test-results"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
ctest --test-dir "${build_dir}" --build-config "${build_type}" \
	--output-on-failure --no-tests=error \
	--output-junit "${build_dir}/test-results/ctest.xml"
