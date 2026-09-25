#!/usr/bin/env python3
"""Read-only live log capture. Stop with SIGTERM; never restart the gateway/agent."""
import argparse
import datetime
import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import signal
import subprocess
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--agent", default="root@10.97.12.49")
    args = parser.parse_args()
    os.umask(0o077)
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    stop = threading.Event()
    children = []
    lock = threading.Lock()

    def shutdown(*_):
        stop.set()
        with lock:
            for child in children:
                if child.poll() is None:
                    child.terminate()

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    def capture(name, command):
        logger = logging.getLogger(name)
        logger.setLevel(logging.INFO)
        handler = RotatingFileHandler(output / f"{name}.log", maxBytes=10 * 1024 * 1024,
                                      backupCount=4, encoding="utf-8")
        handler.setFormatter(logging.Formatter("%(message)s"))
        logger.addHandler(handler)

        def record(line):
            received = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds")
            logger.info("received_utc=%s %s", received, line.rstrip())

        while not stop.is_set():
            try:
                with lock:
                    if stop.is_set():
                        break
                    child = subprocess.Popen(command, stdout=subprocess.PIPE,
                                             stderr=subprocess.STDOUT, text=True, errors="replace")
                    children.append(child)
                record("COLLECTOR connected; timestamp is local receipt time, not device event time")
                for line in child.stdout:
                    record(line)
                record(f"COLLECTOR source exited rc={child.wait()}")
                with lock:
                    children.remove(child)
            except OSError as error:
                record(f"COLLECTOR source error: {error}")
            stop.wait(5)
        handler.close()

    gateway = ["kubectl", "-n", "nanokvm", "logs", "-f", "deployment/nanokvm-gw",
               "--timestamps=true", "--since=30m", "--pod-running-timeout=15s"]
    agent = ["ssh", "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=10",
             "-o", "ServerAliveCountMax=3"]
    if os.environ.get("SSHPASS"):
        agent = ["sshpass", "-e"] + agent + ["-o", "PreferredAuthentications=password",
                                               "-o", "PubkeyAuthentication=no"]
    else:
        agent += ["-o", "BatchMode=yes"]
    agent += [args.agent, "date -u; cat /proc/uptime; tail -n 200 -F /root/nanokvm-rdp/nanokvm-agent.log"]
    threads = [threading.Thread(target=capture, args=(name, cmd))
               for name, cmd in (("gateway", gateway), ("agent", agent))]
    (output / "collector.pid").write_text(f"{os.getpid()}\n")
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()


if __name__ == "__main__":
    main()
