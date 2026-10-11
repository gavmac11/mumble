# Private received-file storage

Each receive job now owns a random QTemporaryDir instead of using the predictable shared `mumble-ft/<transfer-id>` path. Qt creates the private directory atomically; plaintext and password ciphertext spools additionally require owner-only file permissions before use. Invalid storage fails closed. Owned staging is removed on save, abort, failure and normal engine shutdown. Existing saved output and pre-created unrelated paths are preserved.

Three added negative-control cases fail on the original production engine: both storage modes use the predictable path, and a pre-created Unix symlink redirects the decrypted output into a separate private victim fixture. No personal files participate in the control.

The repaired Mac engine passes all 20 cases without skips, including plain/password-spool owner-only permissions, random staging names, pre-created-path preservation, completion, wrong-password/duplicate failure, abort, ready retention and shutdown cleanup. Existing cleanup assertions now locate the actual random staging directory instead of checking a nonexistent old path.

The full repaired client/server build and all 40 enabled local CTest suites pass (41 registered; OverlayTest disabled). See the retained logs. These are current Mac and automated results; Linux/Windows permission and native checks remain pending. A hard process kill cannot run directory cleanup; secure random owner-only residue may survive such a kill and requires a later bounded recovery policy.
