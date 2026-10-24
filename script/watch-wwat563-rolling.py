#!/usr/bin/env python3
"""Detached read-only rolling watcher; never changes receiver configuration."""
import argparse, json, time, urllib.request
from pathlib import Path
import importlib.util

spec=importlib.util.spec_from_file_location("queue",Path(__file__).with_name("append-wwat563-rolling-queue.py"))
queue=importlib.util.module_from_spec(spec); spec.loader.exec_module(queue)

def main():
 p=argparse.ArgumentParser(); p.add_argument("--url",required=True); p.add_argument("--epoch",required=True); p.add_argument("--root",type=Path,required=True); p.add_argument("--interval",type=int,default=60); a=p.parse_args()
 a.root.mkdir(parents=True,exist_ok=True); q=a.root/"queue.jsonl"; status=a.root/"status.json"; frozen=a.root/"frozen-complete"
 while True:
  try:
   with urllib.request.urlopen(a.url,timeout=15) as r: doc=json.load(r)
   known={json.loads(x)["key"] for x in q.read_text().splitlines()} if q.exists() else set()
   new=list(queue.rows(doc,a.epoch,known))
   with q.open("a") as out:
    for row in new: out.write(json.dumps(row,separators=(",",":"))+"\n")
   allrows=[json.loads(x) for x in q.read_text().splitlines()] if q.exists() else []
   # Consumer contract: only promote once frozen marker exists; runners claim
   # a row by writing claims/<key>.json, then completion/<key>.json.
   completed={x.stem for x in (a.root/"completion").glob("*.json")} if (a.root/"completion").exists() else set()
   claimed={x.stem for x in (a.root/"claims").glob("*.json")} if (a.root/"claims").exists() else set()
   for d in (a.root/"claims",a.root/"completion"): d.mkdir(exist_ok=True)
   state="waiting_frozen" if not frozen.exists() else "ready"
   status.write_text(json.dumps({"schema":"vectorwarp-wwat563-rolling-status-v1","updated_at":int(time.time()),"state":state,"appended_last_refresh":len(new),"backlog":len(allrows)-len(completed),"queued":len(allrows)-len(completed)-len(claimed),"running":len(claimed)-len(completed),"completed":len(completed)},separators=(",",":"))+"\n")
  except Exception as e:
   status.write_text(json.dumps({"state":"error","error":str(e),"updated_at":int(time.time())})+"\n")
  time.sleep(max(10,a.interval))
if __name__=="__main__": main()
