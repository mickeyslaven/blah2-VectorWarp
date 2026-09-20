# Scripted macOS release agent

This is a local `launchd` job, not a Codex/AI automation or a GitHub
self-hosted runner. GitHub-hosted macOS 15 runners build and test both runtimes
on every pull request, merge to `main`, and stable version tag. The tag also
creates the eleven Linux packages in a draft release. Every 15 minutes while this
Mac is logged in, the local agent looks for a newer stable draft. It accepts
only a tag on `main` with successful Linux and macOS push runs at the exact
tag commit. It decrypts the two audited CI runtimes, checks corresponding
source and notices, signs with local Developer ID certificates, submits the
package to Apple, staples and verifies it, uploads the Mac PKG/source/receipt,
then publishes the draft and waits for the public download matrix to appear.

The recipient private key and Apple identities stay on this Mac. The agent
refuses a source/recipe/embedded-header/npm-lock change relative to the
reviewed v0.1.9 source bundle. This is intentional: automatically reusing
outdated corresponding source or notices would publish a misleading release.
When that gate fails, the agent logs and notifies the owner; the changed
dependency source/notices must be collected and reviewed before the release
can be published. Homebrew formula updates can trigger this gate even without
a source-code change. The reviewed base bundle covers `cpp-httplib 0.54.1`
on Apple Silicon. The Intel hosted image currently uses `0.53.1`; the agent
checks that CI's formula source URL and SHA-256, fetches that exact source
archive, verifies its MIT license text matches the reviewed notice, and adds
it to the new corresponding-source archive. Other header-version changes,
including a build using `0.56.0`, still stop. Merge builds are unattended, but
tag publication is not guaranteed unattended across future dependency drift.

First-party CMake changes can be reviewed without replacing the dependency
baseline or moving an existing release tag. Review the complete diff from
the baseline commit reported by `prepare-macos-release-inputs.py` to the
candidate tag. Confirm that every guarded change only changes first-party
build wiring and introduces no new dependency source, version, recipe, or
license requirement. If dependency inputs changed, collect and review a new
corresponding-source baseline instead; this mechanism cannot approve them.

Record that review in an owner-only regular JSON file (`chmod 600`) outside
the checkout. Its exact fields are `schema: 1`,
`purpose: "first-party-build-only"`, `baseline_source_id`, `source_id`, and
`files`. Both source IDs must be full commit hashes. `files` maps **every**
guarded changed path to the SHA-256 of that file's Git blob at `source_id`.
Only `CMakeLists.txt` and files immediately inside `cmake/` ending in
`.cmake` are eligible. Use the bytes from `git show COMMIT:PATH`, not a
working-tree copy. Extra or missing paths, a different candidate, an old
baseline, changed hashes, symlinks, or group/world-readable receipts stop
publication. A receipt records a completed human/operator source review;
generating hashes alone does not establish that dependencies are unchanged.

Add the receipt's absolute path as the optional `review_receipt` field in
the existing local agent configuration. Keep the configuration owner-only
and preserve its other fields. The helper runs from the agent's updated,
reviewed `main` checkout; the runtime and archived application source remain
at the immutable release tag. The review applies to that one candidate only.
Remove or replace the configuration field after reviewing a later candidate.
The receipt is copied into the corresponding-source archive as
`VectorWarp-corresponding-source/build-source-review.json`; its digest and
source IDs also appear in `macos-release.json`. Do not put secrets in it.

All original runtime, inventory, header, formula, npm lock, baseline source,
notice, and hosted-build pin checks still run. Cached release inputs repeat
those checks and must match the current review receipt before signing.
Without a receipt, the original fail-closed build-change policy applies.

After this code is merged to `main`, install once on the signing Mac:

```sh
python3 script/install-macos-release-agent.py --check
python3 script/install-macos-release-agent.py
```

The installer copies the reviewed local evidence and encryption key into an
owner-only Application Support directory, clones `main`, writes an owner-only
configuration file and registers
`~/Library/LaunchAgents/com.vectorwarp.macos-release-agent.plist`. It refuses
to overwrite an existing installation. The Mac must be awake, logged in,
online, and have usable GitHub authentication and unlocked Apple signing/
notarization credentials. A failed run is retried at the next interval without
overwriting a published asset.

Inspect the service and logs with:

```sh
launchctl print gui/$(id -u)/com.vectorwarp.macos-release-agent
tail -n 80 "$HOME/Library/Application Support/VectorWarp/ReleaseAgent/logs/stdout.log"
tail -n 80 "$HOME/Library/Application Support/VectorWarp/ReleaseAgent/logs/stderr.log"
```

To run its read-only candidate check immediately:

```sh
python3 "$HOME/Library/Application Support/VectorWarp/ReleaseAgent/repo/script/macos-release-agent.py" \
  --config "$HOME/Library/Application Support/VectorWarp/ReleaseAgent/config.json" --check
```
