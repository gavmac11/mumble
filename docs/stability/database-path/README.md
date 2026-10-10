# Explicit database path isolation

If `misc.database_location` names an existing directory or another path SQLite cannot open, the client used to ignore the failed open and search default locations. A disposable packaged-client harness exposed this by showing the normal user's favorites. That harness launch is excluded from native qualification; startup can maintain database schema/cache and shutdown can vacuum, so absence of deliberate edits does not establish unchanged personal data without a before snapshot.

Initialization now stops with the configured path and SQL error when opening that explicit path fails. An unconfigured database still uses the normal search. Missing configured files retain the existing create/reset/quit choice.

`TestDatabasePath` exercises the actual constructor in child processes. Every child has an explicit private base path and a pre-created disposable fallback, including the failing control. The original constructor fails the configured-directory regression; the repaired constructor passes it, and the fallback database hash remains identical. Valid explicit and default paths also pass, preserving the unselected database hash.

[Before](unit-before.txt): four cases pass and one fails. [After](unit-after.txt): all five pass. A full rebuild then passes [40 enabled CTest suites](local-regression.txt) in 46.98 seconds, with no execution failures/skips; OverlayTest remains disabled (41 registered). [Validation](validation.json) records the scope. Hosted and native Windows/Linux confirmation are still pending, as are the combined frozen candidate and native media/signing acceptance.
