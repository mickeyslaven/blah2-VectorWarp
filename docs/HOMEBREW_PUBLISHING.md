# Public Homebrew tap automation

The public tap is `mickeyslaven/homebrew-vectorwarp`; its Homebrew name is
`mickeyslaven/vectorwarp`. The repository and publishing credential are separate
from the Linux package repositories. The tap is live; updates require the
publishing workflow on `main` and a passing installation check. Local source
installation remains available in [MACOS_HOMEBREW.md](MACOS_HOMEBREW.md).

## After a merge

`.github/workflows/homebrew.yml` runs on a push to `main`, including a PR merge.
It can also be dispatched on `main` to retry the current revision. Pull requests
run offline packaging and publication-policy tests; they cannot publish.

1. Download the exact GitHub archive for the merged 40-character commit SHA.
2. Check its source inventory and generate both formulas with the same source
   URL and SHA-256. The base version is explicitly set in the workflow; the
   formula revision is the full-history Git commit count.
3. On an Apple Silicon runner, install the generated formulas in a temporary
   tap with the real public tap name. Build from source, run both `brew test`
   checks, then installed synthetic replay and lifecycle tests. No physical
   radio is opened and no bottles are produced.
4. In a separate publishing job, download only that run's successful artifact,
   regenerate the formulas independently and compare every generated file.
5. Require the tested source to still be current `main`. Update the tap with an
   ordinary fast-forward push and verify the resulting remote commit. Repeating
   the same source is a no-op only if all managed file hashes match. A newer tap
   version/revision or unrelated source history is never overwritten.

Failed builds leave the public tap unchanged. A run superseded by a later main
commit refuses publication; the later run must pass its own checks. The workflow
does not create a release/tag, publish a Linux package, merge a PR or change
branch protections. Existing Apple Silicon/Intel PR checks remain in `macos.yml`;
the tap's Apple Silicon gate does not establish Intel runtime support.

## Publishing access

The `homebrew-publish` GitHub environment allows only the **branch** `main`.
Its `HOMEBREW_TAP_DEPLOY_KEY` secret contains a dedicated SSH deploy key with
write access only to the tap repository. No personal access token is reused.
The build and pull-request jobs have read-only repository permissions, disable
persisted checkout credentials and cannot access this environment secret.

The publishing step loads the private key into a temporary owner-only directory,
uses explicit key selection without agent fallback, and requires SSH host-key
checking against keys obtained from GitHub's HTTPS metadata endpoint. It removes
that temporary key on exit. The setup receipt records only the public key ID and
fingerprint; the local private key is removed after secret installation.
For the underlying GitHub controls, see
[deploy keys](https://docs.github.com/en/authentication/connecting-to-github-with-ssh/managing-deploy-keys)
and [deployment environments](https://docs.github.com/en/actions/how-tos/deploy/configure-and-manage-deployments/manage-environments).

Do not relax existing source-branch protections or grant this key source-repo
access. Rotate it by replacing the tap deploy key and environment secret
together. An empty new tap is bootstrapped by the first successful bot run;
no manual initial formula commit is required.

## User installation after the first successful publication

```sh
brew tap mickeyslaven/vectorwarp
brew install vectorwarp
vectorwarp
```

The full formula name is `mickeyslaven/vectorwarp/vectorwarp`
(`owner/tap/package`); adding the tap enables the short `vectorwarp` name.
Homebrew builds these formulas from source because no bottles are published.

Use the [Homebrew user guide](MACOS_HOMEBREW.md#public-tap-updates)
or the tap's generated README for updates, optional per-user services and
removal. Existing `vectorwarp/local` users must stop their service and instance,
uninstall both local formulas, then untap `vectorwarp/local` before installing
the public tap. Their application-support configuration and recordings remain.
The old date-based companion version must not be treated as an automatic
cross-tap upgrade.
