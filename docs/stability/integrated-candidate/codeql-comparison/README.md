# CodeQL comparison coverage

At integrated head `873e4a2eb`, CodeQL analysis succeeds, but the combined check is neutral: GitHub cannot determine introduced alerts because the historical matrix configuration is missing. The actual [check warning](prior-check.json) and [master analysis metadata](master-analyses.json) are retained.

Master has both `.github/workflows/code-ql.yml:codeql` (current analysis 1904926966 at 7fc94bf23) and `.github/workflows/code-ql.yml:CodeQL-Build/os:ubuntu-latest` (historical analysis 1692710916 at 2f2beb731). The workflow now runs real traced C++ builds and Python analysis under both identities, using identical permissions and steps through [GitHub-supported YAML anchors and aliases](https://docs.github.com/en/actions/reference/workflows-and-actions/reusing-workflow-configurations). No analysis history is deleted, alert dismissed, language removed or upload fabricated. This adds one complete analysis build per run; after reviewed baseline migration, scan duplication can be reconsidered separately.

Actionlint validates the actual workflow. Hosted analysis and the combined comparison check on the integrated revision remain pending. A successful analysis job alone does not establish clean security acceptance; either configuration may expose unresolved findings.
