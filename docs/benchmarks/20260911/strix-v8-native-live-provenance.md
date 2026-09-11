# Strix v8 native-live provenance

- Live coherent five-channel Kraken receiver input: 527 MHz, 2.4 MS/s; eight
  physical CPUs (0–7), 800% CPU allowance and 100°C campaign limit. Pair cases
  select two channels; only the `array-800hz` cases process all five channels.
- Seven 40-frame cases, with the first eight frames excluded. GPU cases recorded
  30 GPU clutter frames and two CPU oracle frames; GPU ambiguity was active.
- Binary SHA-256: `3d13eb3e578a2b2eb4a7c821d100dc97e4e4163ef59993929f24edced130e36e`.
  GPU module SHA-256: `fa027e7d1362ce7b24bf6f1ac3c0f0756fa3c0152fc8dfecf2cd75c2b11aaa29`.
  Source freeze SHA-256: `8ceb110d4b71854a653789696b17fd7a57a318ab58f6c27212f9899422b666c3`.
- Timing instrumentation adds backend execution fields only; DSP is unchanged.
  Restoration verified unchanged production configuration and CPU limits, and
  that the receiver was stopped after the campaign.
- No sample/drop counter is available. Live mode order does not provide
  identical RF between CPU and GPU runs, so this receipt is not an upstream
  comparison or loss-free-acquisition proof.
