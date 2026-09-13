# Use VectorWarp as a 3lips node

Install [3lips](https://github.com/30hours/3lips#usage) separately and follow its
normal setup. VectorWarp already supplies the radar API it expects.

## 1. Add your VectorWarp nodes

In **3lips's** `config/config.yml`, add each node under `radar`:

```yaml
radar:
  - name: My radar
    url: 192.0.2.10:3000
```

Replace the example IP with your VectorWarp host. Include its port, but **do not
add `http://` or `/api`**. In VectorWarp, check the receiver/transmitter locations
and frequency, enable target detection and ADS-B, then **Save & Restart**.

## 2. Use VectorWarp's built-in ADS-B converter

Stock 3lips calls an external adsb2dd server instead of VectorWarp. That can work
with a reachable public aircraft feed, but not a decoder file or LAN-only feed.

To use our converter, open this file in your **3lips installation**:

`event/algorithm/associator/AdsbAssociator.py`

Replace the whole `generate_api_url` method, stopping before `closest_point`,
with this code. Keep it inside the existing class:

```python
  def generate_api_url(self, radar, radar_data):
    return f"http://{radar}/api/adsb/delay-doppler"
```

This uses each node's configured aircraft feed. **You do not need a separate
adsb2dd service with this change.** It does not change 3lips's separate aircraft
map-feed setting; configure that normally.

## 3. Rebuild and open 3lips

For its standard Docker Compose installation, run from the 3lips directory:

```bash
docker compose up -d --build event
```

Open 3lips on port **49156** and select your radar nodes. VectorWarp itself
still runs without Docker. Keep the radar APIs on a trusted LAN or VPN.

3lips position solving needs at least three suitable receiver/transmitter
geometries. Five antennas at one receiver observing one transmitter count as
one geometry.

This change targets [3lips `897cfdc`](https://github.com/30hours/3lips/blob/897cfdcdf7fc9a922b562bca4238d9729f80f8db/event/algorithm/associator/AdsbAssociator.py)
and was checked with simulated data, not a live multi-node test.
