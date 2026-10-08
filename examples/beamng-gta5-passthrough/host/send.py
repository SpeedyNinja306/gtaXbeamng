"""Send one message to the BeamNG bridge and print any replies for a moment.

  python host/send.py reload
  python host/send.py hud on=false
  python host/send.py debug on=true
  python host/send.py probe

Values are parsed as JSON when they can be (numbers, true/false, lists), otherwise sent as strings.
"""
import json
import socket
import sys
import time

BEAMNG = ("127.0.0.1", 47801)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    msg = {"t": sys.argv[1]}
    for kv in sys.argv[2:]:
        k, _, v = kv.partition("=")
        try:
            msg[k] = json.loads(v)
        except json.JSONDecodeError:
            msg[k] = v
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    sock.settimeout(0.1)
    sock.sendto(json.dumps(msg).encode(), BEAMNG)
    end = time.perf_counter() + 1.0
    while time.perf_counter() < end:
        try:
            data, _ = sock.recvfrom(65535)
        except (socket.timeout, ConnectionResetError):
            continue
        reply = json.loads(data)
        if reply.get("t") != "veh":
            print(reply)


if __name__ == "__main__":
    main()
