# Scripted macOS release agent

This is a local `launchd` job, not a Codex/AI automation or a GitHub
self-hosted runner. GitHub-hosted macOS 15 runners build and test both runtimes
on every pull request, merge to `main`, and stable version tag. A separate,
explicit publish dispatch promotes the exact 24-hour-soaked Linux candidate to a
draft; creating a tag alone does not authorize release. Every 15 minutes while this
Mac is logged in, the local agent looks for a newer stable draft. It accepts
only a tag on `main` with current successful hosted checks, exact-tag Mac runtime
builds, and a successful signing job from that tag's publish dispatch. Older green runs cannot hide
newer failures, and the selected runs are checked again before publication.
It decrypts the two audited CI runtimes, checks corresponding
source and notices, signs with local Developer ID certificates, submits the
package to Apple, staples and verifies it, uploads the Mac PKG/source/receipt,
then publishes the draft and waits for the public download matrix to appear.

The recipient private key and Apple identities stay on this Mac. One current
input manifest binds the exact candidate commit, hosted run, both runtime
manifests, dependency inventories and source closures, compile-header versions,
formula recipes, npm lock, Node pins, and notice inventory. There is no historical
baseline fallback, version-transition exception, network source fetch, or
automatic approval. Both architectures must pass the same closure and license
checks; cached inputs repeat those checks before signing.

After reviewing those actual inputs, create an owner-only regular JSON file
(`chmod 600`) outside the checkout with exactly these fields:

```json
{
  "schema": 1,
  "purpose": "current-macos-release-inputs",
  "source_id": "<full candidate commit>",
  "run_id": "<successful hosted run ID>",
  "version": "<major.minor.patch>",
  "files": {"<input member>": {"sha256": "<digest>", "size": 123}},
  "sources": {}
}
```

The exact required `files` keys come from `input_files()` in
`script/prepare-macos-release-inputs.py`: the source npm lock and Node pin file,
current notices manifest/index, and each architecture's standalone manifest,
inventory, closure, header versions and cpp-httplib formula. `sources` binds
every file in the optional offline public-source directory, using the same
digest/size records. Missing, extra, changed, linked or unreviewed inputs stop
publication. Generating hashes alone does not establish source/license review.
The current notice index must enumerate every production npm dependency's
name/version/license and notice paths in both runtimes, plus the hashed native
notices. The validated notice manifest binds the complete public notice tree.

Configure the absolute paths as `input_manifest`, `current_notices`, and,
when used, `public_sources`. The exact review bytes are archived as
`VectorWarp-corresponding-source/input-review.json`; their digest and candidate
identity appear in the prepared-input and public release receipts. Any changed
candidate or input requires a new review. Preserve old receipts and source
bundles as historical evidence outside the checkout; they are never inputs to
current validation. Existing installations need an explicit reviewed
configuration migration; the installer will not overwrite them.

After this code is merged to `main`, install once on the signing Mac:

```sh
python3 script/install-macos-release-agent.py --check \
  --input-manifest /absolute/current-review.json --notices /absolute/current-notices
python3 script/install-macos-release-agent.py \
  --input-manifest /absolute/current-review.json --notices /absolute/current-notices
```

The installer copies the reviewed local evidence and encryption key into an
owner-only Application Support directory, clones `main`, writes an owner-only
configuration file and registers
`~/Library/LaunchAgents/com.vectorwarp.macos-release-agent.plist`. It refuses
to overwrite an existing installation. The Mac must be awake, logged in,
online, and have usable GitHub authentication and unlocked Apple signing/
notarization credentials. A failed run is retried at the next interval without
overwriting a published asset.

Temporary GitHub or Apple connection failures are logged and retried quietly
at the next interval. They do not produce a macOS attention notification or
mark a release complete. The Git update check is bounded to 60 seconds.
Authentication errors, changed source inputs, signing failures, and other
actionable errors still stop publication and notify the owner. An unchanged
error notifies once until a successful check clears it; a different error
notifies again. A network interruption does not reset that suppression.

Notification state is stored atomically in the owner-only `health.json` beside
the local agent configuration, outside the disposable release cache. Clearing
downloaded build artifacts therefore does not restart a notification loop.
The `--check` command does not update this state or send notifications.

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
