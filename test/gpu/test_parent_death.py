"""The driver worker must die with its parent even while driver work is stuck."""
import os
import select
import subprocess
import sys

for phase in ('orphan-init', 'orphan-frame'):
    parent = subprocess.Popen([sys.argv[1], phase], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True)
    worker_pid = None
    completed = False
    try:
        assert select.select([parent.stdout], [], [], 5)[0], phase + ': worker not ready'
        line = parent.stdout.readline().strip()
        assert line.isdigit(), phase + ': missing worker pid'
        worker_pid = int(line)
        parent.kill()
        # The worker holds both pipe writers. communicate only receives EOF
        # after the orphan worker exits, including when no IPC read is running.
        parent.communicate(timeout=3)
        completed = True
        print('PASS parent-death=' + phase)
    finally:
        if parent.poll() is None:
            parent.kill()
        parent.wait(timeout=3)
        if worker_pid and not completed:
            # Only this fixture's reported worker; cleanup if the assertion fails.
            try:
                os.kill(worker_pid, 9)
            except ProcessLookupError:
                pass
