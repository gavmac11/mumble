# Older-profile automatic folder review

Source `e1d12f7e1140975da419909594127da0e8fa0a70` also addresses the first Opus review's migration concern. Every newly serialized profile records the safe automatic-save policy version, including those with no manual-save history. Loading an enabled automatic-save setting with a missing, old, unknown, string or boolean policy marker turns automatic saving off, preserves the folder and records a diagnostic directing the user to review File Transfer settings. The user can explicitly enable the feature again; save/reload preserves that choice.

73 focused Mac cases and both complete UI/settings serialization suites pass without failures. Full Mac client/server/regression, follow-up actual Opus 5.5 CLI review and native Windows/Linux qualification are pending. The earlier30fa results qualify their recorded source only. This is still a draft; installed GUI and all previously recorded trust/filesystem/media/shipping limits remain open.
