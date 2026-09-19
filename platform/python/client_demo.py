import argparse
import struct
import time

from someip import config, log
from someip.app import ClientV2
from someip.watchdog import Watchdog

SERVICE_ID = 0x1234
INSTANCE_ID = 0x5678

METHOD_GET_VERSION = 0x0001
METHOD_ADD = 0x0002
FIELD_SPEED = 0x1000
EVENT_STATUS = 0x8001
EVENTGROUP_MAIN = 0x0001


def main():
    ap = argparse.ArgumentParser(description="SOME/IP v2 client demo")
    ap.add_argument("-c", "--config", default=None, help="JSON config file")
    args = ap.parse_args()
    cfg = config.load_config(args.config) if args.config else None
    if cfg and cfg["log"]["level"]:
        log.set_default_level(cfg["log"]["level"])
    logger = log.Logger("client_demo")
    kw = config.client_kwargs(cfg) if cfg else {}
    client = ClientV2(**kw).start()
    wd = []

    def on_status(event_id, payload):
        if wd:
            wd[0].pet()
        print(
            "  [event 0x%04X] status: ts=%d speed=%d km/h"
            % (event_id,
               struct.unpack(">II", payload)[0],
               struct.unpack(">II", payload)[1]))

    def on_notify(event_id, payload):
        if wd:
            wd[0].pet()
        print(
            "  [notify 0x%04X] speed field = %d km/h"
            % (event_id, struct.unpack(">I", payload)[0]))

    client.on_event(EVENT_STATUS, on_status)
    client.on_event(FIELD_SPEED + 2, on_notify)

    print("Searching service 0x%04X/0x%04X ..." % (SERVICE_ID, INSTANCE_ID))
    if not client.wait_for_service(SERVICE_ID, INSTANCE_ID,
                                   eventgroups=(EVENTGROUP_MAIN,),
                                   timeout=10):
        print("Service not found, is service_demo.py running?")
        logger.error("service 0x%04X/0x%04X not found within 10s",
                     SERVICE_ID, INSTANCE_ID)
        client.stop()
        return
    ip, port = client.discovered_endpoint(SERVICE_ID, INSTANCE_ID)
    print("Discovered service at %s:%d" % (ip, port))

    rc, payload = client.request(SERVICE_ID, INSTANCE_ID, METHOD_GET_VERSION)
    if rc != 0:
        print("GetVersion failed, return code = 0x%02X" % rc)
        client.stop()
        return
    sid, iid, major, minor = struct.unpack(">IIBB", payload)
    print("GetVersion -> service=0x%04X instance=0x%04X v%d.%d"
          % (sid, iid, major, minor))

    rc, payload = client.request(SERVICE_ID, INSTANCE_ID, METHOD_ADD,
                                 struct.pack(">II", 3, 4))
    result = struct.unpack(">I", payload)[0]
    print("Add(3, 4) -> rc=0x%02X result=%d" % (rc, result))

    rc, payload = client.request(SERVICE_ID, INSTANCE_ID, FIELD_SPEED)
    print("Read Speed  -> rc=0x%02X value=%d km/h"
          % (rc, struct.unpack(">I", payload)[0]))

    if client.subscribe(SERVICE_ID, INSTANCE_ID, (EVENTGROUP_MAIN,)):
        print("Subscribed eventgroup 0x%04X, listening..." % EVENTGROUP_MAIN)
    else:
        print("Subscribe eventgroup failed")

    def on_stall():
        logger.warn("event stream stalled; resubscribing")
        client.resubscribe(SERVICE_ID, INSTANCE_ID)

    wd.append(Watchdog(3.0, on_expired=on_stall, interval=0.2).start())

    rc, payload = client.request(SERVICE_ID, INSTANCE_ID, FIELD_SPEED + 1,
                                 struct.pack(">I", 88))
    print("Write Speed(88) -> rc=0x%02X" % rc)

    rc, payload = client.request(SERVICE_ID, INSTANCE_ID, FIELD_SPEED)
    print("Read Speed  -> rc=0x%02X value=%d km/h"
          % (rc, struct.unpack(">I", payload)[0]))

    try:
        time.sleep(6)
    except KeyboardInterrupt:
        pass
    for w in wd:
        w.stop()
    client.stop()
    print("Done.")


if __name__ == "__main__":
    main()