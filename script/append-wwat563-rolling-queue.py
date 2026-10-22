#!/usr/bin/env python3
"""Append unseen same-config misses without refitting the frozen cohort."""
import argparse, json
from pathlib import Path

def walk(x):
    if isinstance(x, dict):
        yield x
        for v in x.values(): yield from walk(v)
    elif isinstance(x, list):
        for v in x: yield from walk(v)

def rows(stats, epoch, known):
    for e in walk(stats):
        if not (e.get("config_epoch") == epoch and e.get("state") == "finalized" and
                e.get("outcome") == "no_radar_return" and e.get("id")): continue
        for w in e.get("capture_windows", []):
            if w.get("state") != "acknowledged" or not w.get("file"): continue
            key = e["id"] + "|" + w["file"]
            if key in known: continue
            yield {"schema":"vectorwarp-wwat563-rolling-queue-v1","key":key,
                   "event_id":e["id"],"recording":w["file"],"capture":w,
                   "predicted":e.get("predicted",{}),"config_epoch":epoch,
                   "calibration":"frozen-request-cohort","status":"queued"}

def main():
    p=argparse.ArgumentParser(); p.add_argument("--statistics",type=Path,required=True)
    p.add_argument("--epoch",required=True); p.add_argument("--queue",type=Path,required=True)
    a=p.parse_args(); known=set()
    if a.queue.exists():
        known={json.loads(line)["key"] for line in a.queue.read_text().splitlines() if line}
    new=list(rows(json.loads(a.statistics.read_text()),a.epoch,known))
    with a.queue.open("a") as out:
        for row in new: out.write(json.dumps(row,separators=(",",":"))+"\n")
    print(json.dumps({"appended":len(new),"known":len(known),"backlog":len(known)+len(new)}))
if __name__ == "__main__": main()
