# Package acceptance checks

Every release-package job installs its RPM or DEB into a separate, minimal
userspace matching the target OS and architecture. Build dependencies are not
carried into that environment.

The checks start the installed systemd units and verify:

- Fresh installation enables and starts the API without the test starting it; radar processing stays stopped. The API opens with its service account, writable configuration and security restrictions.
- Reinstalling the package removes the known legacy sudo grant while preserving custom policy, saved settings and a running API; it does not start radar processing.
- Chromium opens all six settings tabs, saves settings and reads them back after reload.
- Chromium applies a generated IQ replay through **Save & Restart**. The real API, credential broker and systemd restart the installed non-root processor; the check requires a new API instance, the saved revision and fresh radar frames.
- A missing replay file produces a visible error without reusing old frames. Correcting the path and restarting must produce new frames again.
- Missing SDRplay software produces setup guidance and the official download link, without downloading the SDK.
- Invalid settings and requests from unapproved browser origins cannot change configuration.
- A restart request crosses the real unprivileged-to-root broker boundary; missing receiver software produces an error instead of an endless restart screen.
- The installed processor handles synthetic recording/replay inputs for the supported receiver profiles, including Kraken channel counts from two through eight.
- Real service accounts try to open a synthetic file with the distro's HackRF device permissions. The processor must have access; the web API and unrelated users must not. Debian/Ubuntu retain the vendor `plugdev` group; Fedora uses a narrowly matched HackRF rule.

These checks run on all ten OS/architecture package targets, for PRs as well as
releases, against the newly built package—not a source overlay. The jobs retain
package hashes, installation and service logs, a browser
screenshot, and replay results. Separate broker tests inject denied requests,
timeouts and failed actions under `NoNewPrivileges`; they mock privileged
commands rather than configuring a radio on the runner.

The permission fixture is not a USB device or a udev hotplug test. These checks
do not verify physical USB access, RF reception, a vendor SDK,
GPU drivers, or every kernel and desktop environment. Live receiver tests are
recorded separately. The test containers and browser dependencies are CI tools,
not dependencies of VectorWarp installations.

The disposable test OS needs mount-namespace privileges for systemd. Its outer
container AppArmor/SELinux profile is disabled; it receives no host filesystem
mounts, host namespaces or physical devices. VectorWarp's installed service
accounts and systemd sandbox remain enabled and are checked by the tests. This
is not a test of every host AppArmor/SELinux policy.
The test runtime also supplies systemd's required core-dump hard limit and
waits for its service bus before installing the package; host limits are not changed.

To reproduce the package check on a disposable Linux test host with Podman,
Node 24 and the checked-out source:

```bash
npm ci --prefix test/browser
test/browser/node_modules/.bin/playwright install --with-deps chromium --only-shell
sudo env "PATH=$PATH" bash script/test-installed-package.sh \
  --package /absolute/path/to/vectorwarp.deb \
  --image docker.io/library/ubuntu:22.04 \
  --browser-modules test/browser/node_modules --evidence service-evidence
```

Use the base image matching the package. Set `PLAYWRIGHT_BROWSERS_PATH` to an
explicit shared path when installing the browser and pass that variable through
`sudo` if its default per-user cache is not accessible to the test runner.
