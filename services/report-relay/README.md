# VectorWarp report relay

This Cloudflare Worker accepts an explicitly user-approved, privacy-bounded
VectorWarp diagnostic report and creates an issue in a configured private
GitHub repository. It does not receive continuous telemetry. The public
VectorWarp repository is intentionally not the automatic-report destination.

## Privacy and security properties

- The JSON schema is an allowlist. Unknown fields and sensitive-looking detail
  keys are rejected.
- Free text is bounded and redacts common addresses, endpoints, serials,
  coordinates, credentials, email addresses and local paths.
- The report is sanitized locally by VectorWarp and again by this service.
- Cloudflare's source address is hashed before it is used by the approximate
  rate-limit binding. It is not included in the issue or application logs.
- Worker observability is disabled. Cloudflare still processes ordinary HTTP
  connection metadata as the hosting provider.
- GitHub failures return a generic error and never expose credentials or
  upstream response bodies.
- Each click creates a random submission ID. A Durable Object stores the first
  result for that ID, so network retries return the same receipt without
  opening duplicate issues. The coarser incident fingerprint is used only to
  correlate similar failures and is not the receipt shown to the user.
- If GitHub accepts a report but its response is lost, the durable pending
  record is reconciled against the exact submission marker before any retry;
  an ambiguous retry never performs a second issue-creation request.
- The endpoint fails closed if either rate limiting or deduplication is not
  configured.

## Local verification

```sh
cd services/report-relay
npm test
```

The tests mock GitHub. They do not need a token and do not make network calls.

After deployment, the explicitly gated live pathway smoke test creates twelve
synthetic issues at a rate below the production limiter. Run it only against a
private test/triage destination, verify the issues, and close them afterward:

```sh
ALLOW_LIVE_REPORT_TEST=1 \
REPORT_RELAY_URL=https://vectorwarp-report-relay.example.workers.dev \
npm run test:live-pathways
```

## Deployment

1. Create a private diagnostic-report repository. Its issues and membership
   must not be public.
2. Create a fine-grained GitHub token limited to that repository with
   `Issues: write`. A GitHub App installation token can replace it later.
3. Ensure the private repository has the `bug` and `vectorwarp-report` labels.
   Review the rate-limit namespace and Durable Object migration in
   `wrangler.jsonc`; both bindings are mandatory.
4. Store both the token and private destination as Worker secrets; never put
   either in the config or source:

   ```sh
   npx wrangler@latest secret put GITHUB_TOKEN
   npx wrangler@latest secret put GITHUB_REPOSITORY
   npm run deploy
   ```

   `GITHUB_REPOSITORY` uses the `owner/repository` form.

5. Send a synthetic report and verify that the private issue was created before
   putting the relay URL into VectorWarp. The client receives only a report ID;
   it never receives or exposes the private issue URL.

The public endpoint is `POST /v1/reports`; it requires
`X-VectorWarp-Report: 1`. `GET /health` is a credential-free health check. The
Worker intentionally permits cross-origin requests because VectorWarp is
commonly served from private LAN origins. The marker header is defense in
depth, not authentication; validation and rate limiting remain the security
boundary.
