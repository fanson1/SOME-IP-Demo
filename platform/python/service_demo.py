import argparse
import struct
import time

from someip import config, log
from someip.app import SomeipServiceV2

SERVICE_ID = 0x1234
INSTANCE_ID = 0x5678

METHOD_GET_VERSION = 0x0001
METHOD_ADD = 0x0002
FIELD_SPEED = 0x1000
EVENT_STATUS = 0x8001
EVENTGROUP_MAIN = 0x0001


def main():
    ap = argparse.ArgumentParser(description="SOME/IP v2 service demo")
    ap.add_argument("-c", "--config", default=None, help="JSON config file")
    args = ap.parse_args()
    cfg = config.load_config(args.config) if args.config else None
    if cfg and cfg["log"]["level"]:
        log.set_default_level(cfg["log"]["level"])
    logger = log.Logger("service_demo")
    kw = config.service_kwargs(cfg) if cfg else {}
    service = SomeipServiceV2(
        service_id=kw.pop("service_id", SERVICE_ID),
        instance_id=kw.pop("instance_id", INSTANCE_ID),
        major_version=kw.pop("major_version", 0x01),
        minor_version=kw.pop("minor_version", 0x00000001),
        **kw,
    )

    def get_version(_payload, _addr):
        return 0x00, struct.pack(">IIBB", SERVICE_ID, INSTANCE_ID, 1, 0)

    def add(payload, _addr):
        if len(payload) != 8:
            return 0x09, b""
        a, b = struct.unpack(">II", payload)
        return 0x00, struct.pack(">I", a + b)

    service.add_method(METHOD_GET_VERSION, get_version)
    service.add_method(METHOD_ADD, add)
    service.add_uint32_field(FIELD_SPEED, EVENTGROUP_MAIN, initial=0)
    service.add_event(EVENT_STATUS, EVENTGROUP_MAIN)

    service.start()
    print("SOME/IP Service (v2) started:")
    print("  service_id    = 0x%04X" % SERVICE_ID)
    print("  instance_id   = 0x%04X" % INSTANCE_ID)
    print("  method        = udp %s:%d" % (service.interface_ip,
                                           service.method_port))
    print("  event         = udp %s:%d" % (service.interface_ip,
                                           service.event_port))
    print("  sd            = %s:%d" % ("224.244.224.245", service.sd_port))
    print("  methods       = GetVersion(0x0001), Add(0x0002)")
    print("  field         = Speed(0x1000, getter/setter/notifier)")
    print("  event         = Status(0x8001)")
    print("  eventgroup    = 0x0001")
    print("Waiting for clients... (Ctrl+C to quit)")
    logger.info("listening method=%d event=%d sd=%d",
                service.method_port, service.event_port, service.sd_port)

    speed = 0
    try:
        while True:
            time.sleep(1)
            speed = (speed + 10) % 220
            service.set_field(FIELD_SPEED, speed)
            payload = struct.pack(">II", int(time.time()), speed)
            service.publish_event(EVENT_STATUS, payload)
    except KeyboardInterrupt:
        print("\nStopping service...")
        logger.info("stopping service")
        service.stop()


if __name__ == "__main__":
    main()