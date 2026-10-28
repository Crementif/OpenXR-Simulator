"""Exercise the real inherited pipe without a physical OpenXR runtime."""
import subprocess
import sys

probe = sys.argv[1]
for close in (False, True):
    with subprocess.Popen([probe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE) as child:
        assert child.stdout.readline().rstrip(b'\r\n') == b'ready', 'Idle pipe failed or blocked'
        assert child.poll() is None, 'Idle parent connection must keep the host alive'
        if close:
            child.stdin.close()
        else:
            child.stdin.write(b'stop\n')
            child.stdin.flush()
        assert child.wait(timeout=10) == 0, child.stderr.read().decode()
rejected = subprocess.run([probe], stdin=subprocess.DEVNULL, capture_output=True, timeout=10)
assert rejected.returncode == 1 and b'standard-input pipe' in rejected.stderr
print('Parent control idle, explicit stop, writer closure and invalid input checks passed')
