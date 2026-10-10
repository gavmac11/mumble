# Client releases and updates

Every successful `master` push in **Preview installers** publishes a new prerelease
after the Windows, macOS, Ubuntu client, Debian server, and publication-test jobs pass.
Each platform runs CTest from the same build used for its package; missing or empty
test suites fail the job. JUnit reports and CTest logs are retained even after failures.
The general build workflow also runs tests on Windows x64, Ubuntu x64, and both
Apple Silicon and Intel macOS. These checks do not replace native GUI acceptance.
The release tag, client version, and installer version all use `1.7.<workflow run number>`.
PRs also consume run numbers, so gaps are expected. Concurrent pushes may cancel superseded
builds; only completed builds are released. Minor/major version changes remain deliberate.
The numeric components must fit in 16 bits; bump the version series before exhausting it.

The publisher creates a tag at the exact built commit, creates a draft, uploads all four
packages and SHA-256 checksums, then publishes. Rerunning the same workflow can finish an
interrupted draft. It never moves a tag or replaces assets in an already published release.
Authentication errors and tag collisions fail publication. The legacy `master-preview`
release is retained as history and is no longer refreshed.

## Client behavior

The checker uses the public GitHub releases API for `gavmac11/mumble`. For previews, it selects the highest
numeric `vMAJOR.MINOR.PATCH` version among the 100 most recent releases, ignoring drafts,
legacy tags, incomplete packages, and unrelated download URLs. It compares against the version
compiled into the client, not the displayed release title or publication date.

Preview builds accept both prereleases and stable releases. For a stable-only build, configure
`-Dupdate-prereleases=OFF`. Stable releases use the same numeric tag format with GitHub's
prerelease flag unset. Publishing or promoting a stable release is a maintainer decision;
merges never automatically enter the stable channel. Stable-only builds use GitHub's latest
stable release endpoint, so previews cannot hide a stable release.

A persistent cache stores each channel's validated response and ETag. Conditional requests reuse
that response on HTTP 304. Automatic checks run at most once per six hours per local cache,
even after failed attempts or restarts. Manual checks bypass that interval. Both respect
GitHub's `Retry-After` or `X-RateLimit-Reset` on HTTP 403/429 (with a one-hour fallback and
24-hour maximum). Cache writes are atomic; malformed responses cannot replace a good cache.
ETags reduce transfer size, but unauthenticated conditional requests still consume GitHub's
per-IP rate limit; the interval and backoff reduce that exposure without embedding credentials.

Automatic checks respect the existing update setting and remain quiet on network errors,
empty feeds, and equal/older versions. Manual checks explain these outcomes. Checks time out,
bound response size, honor the configured proxy and OS privacy preference, and do not hash or
upload the client executable. Unsupported platforms receive a release-notes link instead of
an incompatible installer. The Ubuntu download is only offered on Ubuntu 24.04 amd64; the macOS ARM64 download requires macOS 15 or newer.

Downloads open in the browser and require manual installation. The current Windows installer
is unsigned and the macOS bundle is ad-hoc signed; unattended replacement should wait for a
signed and verified update mechanism. Older clients must be updated manually once to receive
the new checker. To roll back, install a previous version from the releases page.

## Validation

`TestReleaseUpdate` exercises channel filtering, numeric ordering, asset selection, malformed
feeds, hostile download links, persistent caching, and request backoff. The standalone target
uses the same Qt string-builder definitions as full builds. It runs with the client CTest suite. Run the publisher tests
without contacting GitHub:

```sh
python3 -m unittest discover -s scripts/tests -p 'test_publish_preview.py'
```
