"""Build-independent Docker smoke check, including open stdin and HTTP routes."""
import json
import socket
import subprocess
import sys
import time
import urllib.request
import uuid

image = sys.argv[1]
name = f"hft-smoke-{uuid.uuid4().hex[:12]}"
with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    port = listener.getsockname()[1]
process = subprocess.Popen(
    ["docker", "run", "--rm", "-i", "--name", name,
     "-p", f"127.0.0.1:{port}:9100", image, "--config", "/etc/hft/hft_feed.conf", "--stdin"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
)
try:
    process.stdin.write(b"1000000000,NEW,BID,1,100,1,100,101\n")
    process.stdin.flush()
    deadline = time.monotonic() + 20
    while True:
        if process.poll() is not None:
            raise RuntimeError(f"container exited: {process.returncode}")
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=2) as response:
                health = json.load(response)
            if health["events"] == 1:
                break
        except OSError:
            pass
        if time.monotonic() > deadline:
            raise RuntimeError("container did not process open-stdin event")
        time.sleep(0.1)
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/metrics", timeout=2) as response:
        if "hft_events_total 1\n" not in response.read().decode():
            raise RuntimeError("missing event metric")
    subprocess.run(["docker", "stop", "--time", "3", name], check=True, timeout=10)
    if process.wait(timeout=5) != 0:
        raise RuntimeError("container did not stop cleanly")
    print("container stdin, health, metrics, shutdown: ok")
finally:
    subprocess.run(["docker", "rm", "-f", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    process.stdin.close()
    if process.poll() is None:
        process.kill()
    process.wait()
