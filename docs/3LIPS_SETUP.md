# Using VectorWarp with 3lips

[3lips](https://github.com/30hours/3lips) combines observations from multiple
passive-radar geometries to estimate target positions. VectorWarp retains the
blah2 detection and configuration interfaces it reads. However, **stock 3lips
does not use VectorWarp's built-in ADS-B converter**; its external adsb2dd
dependency still needs to be addressed.

## Prepare the radar nodes

1. On each VectorWarp node, set **Settings → Sites** to the actual receiver and
   illuminator positions, including elevation. Check the center frequency under
   **Receiver** and enable **Radar → Detection → Detect targets**.
2. Choose **Save & Restart** and confirm fresh detections. Saving settings for
   later is not enough: 3lips reads the saved configuration, so it must match
   the running processor.
3. From the machine running 3lips, check each node's `/api/config` and
   `/api/detection` endpoints. The default VectorWarp web/API port is 3000.
   For example, replacing this documentation address with your node's address:

   ```bash
   curl --fail http://192.0.2.10:3000/api/config
   curl --fail http://192.0.2.10:3000/api/detection
   ```

Detection output contains delay in kilometres of **excess path**, Doppler in
Hz, SNR, and a millisecond timestamp. Keep the hosts' clocks synchronized.
Use a trusted LAN or VPN; do not expose VectorWarp's unauthenticated settings
API to the public Internet just to connect 3lips.

The current 3lips localization path requires associated detections from at least
three radar geometries. Five Kraken channels at one receiver, observing one
illuminator, are still **one geometry**, not five independent radar nodes.

## Configure 3lips

Install 3lips separately using its [upstream instructions](https://github.com/30hours/3lips#usage).
Its stock deployment uses Docker Compose; VectorWarp itself remains native.
In the **3lips** `config/config.yml`, replace the `radar` list with your nodes:

```yaml
radar:
  - name: Radar A
    url: 192.0.2.10:3000
  - name: Radar B
    url: 192.0.2.11:3000
  - name: Radar C
    url: 192.0.2.12:3000
```

These are example addresses. Use addresses reachable from the 3lips event
process, including from inside its container if applicable. Omit `http://`
and `/api` here: stock 3lips adds them. Keep the other configuration sections,
set the map position for your area, and configure its separate `map.tar1090`
aircraft feed. The current map-truth reader adds `https://` and
`/data/aircraft.json` to that feed address.

After starting 3lips, open its interface on port 49156, select your radar nodes,
ADS-B association, and a localization method suitable for their geometry.
Correct API responses alone do not establish that enough aircraft are detected
across the nodes to produce positions.

## The ADS-B limitation

VectorWarp converts its configured local or remote aircraft feed internally
and serves the result at **`/api/adsb/delay-doppler`**. Stock 3lips ignores
that endpoint. Instead, its associator reads each radar's
`truth.adsb.tar1090` value and sends a request to the hardcoded external service
**`http://adsb2dd.30hours.dev/api/dd`**.

This has three practical consequences:

- The external service must be available and able to reach the aircraft feed.
  It cannot read a local decoder file or reach a LAN-only server.
- Stock 3lips prepends `http://` to that setting. A full URL accepted by
  VectorWarp, such as `http://receiver/tar1090`, becomes an invalid doubled-scheme
  URL in that request.
- A working aircraft overlay in VectorWarp does not mean ADS-B association
  will work in 3lips. 3lips also has the separate map feed described above.

For a local-only setup, 3lips needs an integration change to read each node's
built-in delay–Doppler endpoint, or a configurable, reachable separate converter.
**Neither change is included in VectorWarp.** Do not make a private feed public
to work around this. 3lips uses ADS-B to associate detections; VectorWarp still
uses ADS-B only for display and evaluation, not radar detection or tracking.

## Change the delay coverage

In VectorWarp, open **Settings → Radar → Search area**, edit **Minimum delay
bin** and **Maximum delay bin**, then choose **Save & Restart**. These controls
change the calculated delay range, not just the plot zoom. Validation checks
the limits against the sample rate, CPI and Doppler span.

One bin represents `299792458 / sample_rate` metres of excess path. At
2.4 MS/s, maximum bin 245 is about 30.6 km. This is the extra transmitter–target–
receiver path compared with the direct path, **not distance from the receiver**.
Increasing coverage can increase processing and output work.

## Compatibility check

This guide was checked against the VectorWarp API/UI source and
[3lips at `897cfdc`](https://github.com/30hours/3lips/tree/897cfdcdf7fc9a922b562bca4238d9729f80f8db),
particularly its [event loop](https://github.com/30hours/3lips/blob/897cfdcdf7fc9a922b562bca4238d9729f80f8db/event/event.py),
[ADS-B associator](https://github.com/30hours/3lips/blob/897cfdcdf7fc9a922b562bca4238d9729f80f8db/event/algorithm/associator/AdsbAssociator.py),
and [map-truth reader](https://github.com/30hours/3lips/blob/897cfdcdf7fc9a922b562bca4238d9729f80f8db/event/algorithm/truth/AdsbTruth.py).
It is an interface review, not an end-to-end multi-node hardware test.
