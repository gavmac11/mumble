# Recovery fixture certificate-ban identifier

The remaining combined CodeQL annotation at integrated head `101d94a0f` concerns computing SHA1 over a certificate in the fixture. The fixture now consumes the connected user’s `UserState.hash` identifier supplied by the authenticated server, as native ban administration does. Missing or malformed identifiers fail closed. CA validation and the SHA256 server-identity comparison remain in place; the legacy server protocol is unchanged.

All 62 hosting checks pass without skips, including missing/malformed/wrong-session identifiers. An isolated modern loopback server passes actual account registration, channel/group/ACL allow and deny checks, persisted ban lookup and certificate-based login denial in both seed and check modes. Historical-server qualification and a fresh combined security check remain open.

An initial fixture-server setup occurred before its default instance existed and its first login was rejected. The owned instance was initialized, its password set through standard input, and the retained successful checks ran afterward. No unrelated server or user profile was involved.
