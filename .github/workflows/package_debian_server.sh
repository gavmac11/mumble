#!/usr/bin/env bash

set -euo pipefail

workspace="${1:?workspace is required}"
version="${2:?package version is required}"
architecture="${3:-amd64}"

package_dir="${workspace}/build/debian-package"
package_root="${package_dir}/root"
dist_dir="${workspace}/dist"
package_name="mumble-server"
artifact_name="Mumble-Debian-12-${architecture}.deb"

rm -rf "${package_dir}"
mkdir -p "${package_root}/DEBIAN" "${dist_dir}"

DESTDIR="${package_root}" cmake --install "${workspace}/build" --component mumble_server
DESTDIR="${package_root}" cmake --install "${workspace}/build" --component doc

install -Dm644 "${workspace}/LICENSE" "${package_root}/usr/share/doc/${package_name}/copyright"

mkdir -p "${package_dir}/debian"
cat > "${package_dir}/debian/control" <<EOF
Source: ${package_name}
Section: sound
Priority: optional
Maintainer: Mumble Preview Builds <noreply@github.com>
Standards-Version: 4.6.2

Package: ${package_name}
Architecture: ${architecture}
Description: Low-latency voice chat server (master preview)
 This package contains a preview build of the Mumble server from the master
 branch. It is intended for testing and may be unstable.
EOF

dependency_output="$(
  cd "${package_dir}"
  dpkg-shlibdeps -O -e"${package_root}/usr/bin/mumble-server"
)"
runtime_dependencies="${dependency_output#shlibs:Depends=}"

installed_size="$(du -sk "${package_root}" | cut -f1)"
cat > "${package_root}/DEBIAN/control" <<EOF
Package: ${package_name}
Version: ${version}
Section: sound
Priority: optional
Architecture: ${architecture}
Maintainer: Mumble Preview Builds <noreply@github.com>
Installed-Size: ${installed_size}
Depends: ${runtime_dependencies}, systemd
Homepage: https://www.mumble.info
Description: Low-latency voice chat server (master preview)
 This package contains a preview build of the Mumble server from the master
 branch. It is intended for testing and may be unstable.
EOF

cat > "${package_root}/DEBIAN/conffiles" <<'EOF'
/etc/mumble/mumble-server.ini
EOF

cat > "${package_root}/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e

if command -v systemd-sysusers >/dev/null 2>&1; then
	systemd-sysusers /usr/lib/sysusers.d/mumble-server.conf
fi

if command -v systemd-tmpfiles >/dev/null 2>&1; then
	systemd-tmpfiles --create /usr/lib/tmpfiles.d/mumble-server.conf
fi

if command -v systemctl >/dev/null 2>&1; then
	systemctl daemon-reload >/dev/null 2>&1 || true
fi
EOF

cat > "${package_root}/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e

if command -v systemctl >/dev/null 2>&1; then
	systemctl daemon-reload >/dev/null 2>&1 || true
fi
EOF

chmod 0755 "${package_root}/DEBIAN/postinst" "${package_root}/DEBIAN/postrm"

dpkg-deb --root-owner-group --build "${package_root}" "${dist_dir}/${artifact_name}"
dpkg-deb --info "${dist_dir}/${artifact_name}"
dpkg-deb --contents "${dist_dir}/${artifact_name}"
sha256sum "${dist_dir}/${artifact_name}" > "${dist_dir}/${artifact_name}.sha256"
