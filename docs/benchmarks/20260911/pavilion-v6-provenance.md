# Pavilion v6 provenance

This compact receipt accompanies the aggregate CSV. It intentionally omits local
paths, host addresses and full logs; those remain with the campaign evidence.

- Baseline: unmodified `30hours/blah2` commit
  `c821bee3f0d27cf20c8447f3d908ef722905a4de`.
- Corrected VectorWarp GPU source: `407b9737c744f2ef754139767adad238d25d52f0`.
- Input: four seconds of five-channel recorded IQ, SHA-256
  `89bfe1c338995167946b7c367b80480143b60a515d9eabde39380bcc48e03a5d`.
- Runs: two alternating 20-frame repeats per engine/device; exclude frames 0–7,
  so each aggregate has 24 steady frames. Sample-clock pacing preserves overload
  as lag rather than dropping input.
- Geometry: 527 MHz, 2.4 MS/s, 200 ms CPI, delay −10…245, clutter −10…200.
  Pair profiles use one worker/four FFT threads; the five-channel array uses four
  workers/one FFT thread. Doppler windows are ±800, ±2400 and ±4000 Hz.
- Devices: Intel HD Graphics 630 (KBL GT2) and AMD Radeon 500 Series (RADV
  POLARIS12). CPU critical limit was 100°C; AMD edge critical limit was 94°C.
  Active peaks were 83°C CPU package and 54°C AMD edge.
- Accuracy: every Pavilion v6 row passed the campaign comparison. GPU complex
  maps were within the 1e-4 relative RMS/peak acceptance tolerance; this is not
  bit-exact output or a claim that detection SNR is identical.
- GPU scope: Vulkan delay–Doppler and clutter FFTs run on GPU; clutter FP64
  solve and the remaining stages run on CPU. Each steady GPU window has eleven
  GPU frames and one periodic CPU oracle frame.
