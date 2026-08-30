#!/usr/bin/env bash

set -euo pipefail

workspace="${1:?workspace is required}"
version="${2:?package version is required}"
architecture="${3:-amd64}"

package_dir="${workspace}/build/ubuntu-client-package"
package_root="${package_dir}/root"
dist_dir="${workspace}/dist"
package_name="mumble"
artifact_name="Mumble-Ubuntu-24.04-${architecture}.deb"

rm -rf "${package_dir}"
mkdir -p "${package_root}/DEBIAN" "${dist_dir}"

DESTDIR="${package_root}" cmake --install "${workspace}/build"
install -Dm644 "${workspace}/LICENSE" "${package_root}/usr/share/doc/${package_name}/copyright"

if [[ ! -x "${package_root}/usr/bin/mumble" ]]; then
	echo "The staged package does not contain /usr/bin/mumble." >&2
	exit 1
fi

mkdir -p "${package_dir}/debian"
cat > "${package_dir}/debian/control" <<EOF
Source: ${package_name}
Section: sound
Priority: optional
Maintainer: Mumble Preview Builds <noreply@github.com>
Standards-Version: 4.6.2

Package: ${package_name}
Architecture: ${architecture}
Description: Low-latency voice chat client (master preview)
 This package contains a preview build of the Mumble client from the master
 branch. It is intended for testing and may be unstable.
EOF

dependency_args=()
while IFS= read -r -d '' candidate; do
	if file "${candidate}" | grep -Eq 'ELF .* (dynamically linked|shared object)'; then
		dependency_args+=("-e${candidate}")
	fi
done < <(find "${package_root}/usr" -type f -print0)

if (( ${#dependency_args[@]} == 0 )); then
	echo "No dynamically linked ELF files were found in the staged package." >&2
	exit 1
fi

dependency_output="$(
	cd "${package_dir}"
	dpkg-shlibdeps \
		--ignore-missing-info \
		-l"${package_root}/usr/lib/mumble" \
		-l"${package_root}/usr/lib/mumble/plugins" \
		-O \
		"${dependency_args[@]}"
)"
runtime_dependencies="${dependency_output#shlibs:Depends=}"

if [[ -z "${runtime_dependencies}" || "${runtime_dependencies}" == "${dependency_output}" ]]; then
	echo "Unable to determine package runtime dependencies." >&2
	exit 1
fi

installed_size="$(du -sk "${package_root}" | cut -f1)"
cat > "${package_root}/DEBIAN/control" <<EOF
Package: ${package_name}
Version: ${version}
Section: sound
Priority: optional
Architecture: ${architecture}
Maintainer: Mumble Preview Builds <noreply@github.com>
Installed-Size: ${installed_size}
Depends: ${runtime_dependencies}
Homepage: https://www.mumble.info
Description: Low-latency voice chat client (master preview)
 This package contains a preview build of the Mumble client from the master
 branch. It is intended for testing and may be unstable.
EOF

dpkg-deb --root-owner-group --build "${package_root}" "${dist_dir}/${artifact_name}"
dpkg-deb --info "${dist_dir}/${artifact_name}"
dpkg-deb --contents "${dist_dir}/${artifact_name}"
(
	cd "${dist_dir}"
	sha256sum "${artifact_name}" > "${artifact_name}.sha256"
)
