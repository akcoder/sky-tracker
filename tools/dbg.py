#!/usr/bin/env python3
"""Drive the unit without a finger and read what the screen cost (PERF-19/20), over the network.

It calls the unit's API action debug_cmd (a command line) and reads the unit's log stream, so nothing touches the
serial port (opening it resets the board) and nothing reboots.

  tools/dbg.py [--host 192.168.0.217] script.txt

Script lines:  wait <seconds>   |   # comment   |   anything else is sent as a command:
  perf | reset | close | open <sun|moon|earth|region|uaf|planet N> | page <sky|list|settings|about|debug>
Every perf / dbg / "took a long time" log line is printed as it arrives. Needs aioesphomeapi (the ESPHome venv has it)."""
import argparse, asyncio, re, time

from aioesphomeapi import APIClient, LogLevel


async def main(a):
    cli = APIClient(a.host, 6053, None)
    await cli.connect(login=True)
    _, services = await cli.list_entities_services()
    svc = next(s for s in services if s.name == "debug_cmd")
    t0 = time.time()

    def on_log(msg):
        t = re.sub(r"\x1b\[[0-9;]*m", "", msg.message.decode("utf-8", "replace"))
        if "perf" in t or "dbg" in t or "took a long time" in t:
            print(f"  {time.time() - t0:7.1f}s  {t[:230]}", flush=True)

    cli.subscribe_logs(on_log, log_level=LogLevel.LOG_LEVEL_DEBUG)
    for raw in open(a.script):
        cmd = raw.strip()
        if not cmd:
            continue
        if cmd.startswith("#"):
            print(cmd, flush=True)
        elif cmd.startswith("wait "):
            await asyncio.sleep(float(cmd.split()[1]))
        else:
            print(f"> {cmd}", flush=True)
            await cli.execute_service(svc, {"cmd": cmd})
            await asyncio.sleep(1.2 if cmd == "perf" else 0.5)
    await cli.disconnect()


ap = argparse.ArgumentParser()
ap.add_argument("script")
ap.add_argument("--host", default="192.168.0.217")
asyncio.run(main(ap.parse_args()))
