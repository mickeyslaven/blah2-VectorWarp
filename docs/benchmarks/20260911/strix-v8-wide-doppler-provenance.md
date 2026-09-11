# Strix v8 wide-Doppler provenance

Archived, ineligible for current performance claims: these tests shortened the
excess-path window. Use the [fixed-range campaign](../20260911-equal-range/README.md).

- VectorWarp source commit: `907e467`; source freeze SHA-256:
  `8ceb110d4b71854a653789696b17fd7a57a318ab58f6c27212f9899422b666c3`.
- Input: the recorded five-channel IQ window used by the matched campaign,
  527 MHz and 2.4 MS/s. Pair runs use one worker/eight FFT threads; arrays use
  four workers/two FFT threads.
- Each result aggregates two 20-frame runs after exclusion of eight startup
  frames per run. The GPU steady window contains eleven GPU frames and one CPU
  oracle frame. GPU maps passed the campaign tolerance, not bit-exact comparison.
- Regular upstream blah2 was not timed for these one-second stress geometries:
  source-derived preflight marked them unsafe. CPU rows are VectorWarp CPU rows.
- The ±40 kHz cases use 32 bins (delay −10…21); ±20 kHz uses 64 bins
  (delay −10…53). These are processing-capacity tests on recorded VHF IQ, not
  a higher-frequency receiver, illuminator, link-budget or detection validation.
