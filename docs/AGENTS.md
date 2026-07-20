# Documentation instructions

## Folly guide

These instructions apply to `folly-library-guide.md` and every document under
`folly/`. Treat the collection as a standalone, generally applicable guide to
Meta Folly.

- Base its scope on Folly's public library and official upstream sources, not
  on applications, examples, dependencies, build configuration, or installed
  versions found in this workspace.
- Do not mention, link to, analyze, or derive examples from code elsewhere in
  this workspace.
- Do not add local filesystem paths, workspace-relative links outside the
  guide, or statements such as "in this repository", "the local example", or
  "the installed version".
- Keep examples generic and self-contained. Use invented domain names where a
  scenario helps explain an API.
- Describe version-sensitive APIs as such and direct readers to the official
  upstream header or documentation. References to the official Facebook Folly
  repository are allowed and encouraged.
- Do not narrow the guide merely because only a subset of Folly is exercised by
  code in this workspace.
- Before completing an edit, search the guide for local paths and
  workspace-specific references and remove them.

