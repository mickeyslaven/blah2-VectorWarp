# Package acceptance checks

Every release-package job installs its RPM or DEB into a separate, minimal
userspace matching the target OS and architecture. Build dependencies are not
carried into that environment.

The checks start the installed systemd units and verify:

- The API opens with its service account, writable configuration and security restrictions.
- Reinstalling the package removes the known legacy sudo grant while preserving custom policy, saved settings and a running API; it does not start radar processing.
- Chromium opens all six settings tabs, saves a frame-interval change and reads it back after reload.
- Missing SDRplay software produces setup guidance and the official download link, without downloading the SDK.
- Invalid settings and requests from unapproved browser origins cannot change configuration.
- A restart request crosses the real unprivileged-to-root broker boundary; missing receiver software produces an error instead of an endless restart screen.
- The installed processor handles synthetic recording/replay inputs for the supported receiver profiles, including Kraken channel counts from two through eight.

The jobs retain package hashes, installation and service logs, a browser
screenshot, and replay results. Separate broker tests inject denied requests,
timeouts and failed actions under `NoNewPrivileges`; they mock privileged
commands rather than configuring a radio on the runner.

These checks do not verify physical USB access, RF reception, a vendor SDK,
GPU drivers, or every kernel and desktop environment. Live receiver tests are
recorded separately. The test containers and browser dependencies are CI tools,
not dependencies of VectorWarp installations.

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
