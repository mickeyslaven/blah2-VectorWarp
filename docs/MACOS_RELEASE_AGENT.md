# Scripted macOS release agent

This is a local `launchd` job, not a Codex/AI automation or a GitHub
self-hosted runner. GitHub-hosted macOS 15 runners build and test both runtimes
on every pull request, merge to `main`, and stable version tag. The tag also
creates the ten Linux packages in a draft release. Every 15 minutes while this
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
a source-code change. In particular, the reviewed bundle records
`cpp-httplib 0.54.1`; a build using `0.56.0` will stop here. Thus merge builds
are unattended, but tag publication is not guaranteed unattended across
dependency drift.

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
