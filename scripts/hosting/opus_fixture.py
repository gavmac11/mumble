"""Read a small, single-stream Ogg Opus fixture produced by FFmpeg.

This is a bounded test-fixture reader, not a general Ogg demuxer.
TOC durations follow RFC 6716 section 3.1.
"""


def packet_duration_ms(packet):
    if not packet:
        raise ValueError("empty Opus packet")
    config = packet[0] >> 3
    if config < 12:
        frame_ms = (10, 20, 40, 60)[config % 4]
    elif config < 16:
        frame_ms = (10, 20)[config % 2]
    else:
        frame_ms = (2.5, 5, 10, 20)[config % 4]
    code = packet[0] & 3
    if code == 3:
        if len(packet) < 2:
            raise ValueError("missing Opus frame count")
        count = packet[1] & 63
    else:
        count = (1, 2, 2)[code]
    duration = frame_ms * count
    if not 0 < duration <= 120:
        raise ValueError("invalid Opus packet duration")
    return duration


def read_packets(data):
    if not data or len(data) > 512 * 1024:
        raise ValueError("Opus fixture must contain 1 byte to 512 KiB")
    offset = 0
    serial = None
    page_number = 0
    pending = bytearray()
    packets = []
    while offset < len(data):
        header = data[offset:offset + 27]
        if len(header) != 27 or header[:5] != b"OggS\x00":
            raise ValueError("invalid Ogg page header")
        current_serial = header[14:18]
        if serial is None:
            serial = current_serial
        if current_serial != serial or int.from_bytes(header[18:22], "little") != page_number:
            raise ValueError("fixture must be one ordered Ogg stream")
        if bool(header[5] & 1) != bool(pending):
            raise ValueError("invalid Ogg continuation")
        page_number += 1
        offset += 27
        segments = data[offset:offset + header[26]]
        if len(segments) != header[26]:
            raise ValueError("truncated Ogg segment table")
        offset += len(segments)
        for size in segments:
            fragment = data[offset:offset + size]
            if len(fragment) != size:
                raise ValueError("truncated Ogg payload")
            pending.extend(fragment)
            offset += size
            if size < 255:
                packets.append(bytes(pending))
                pending.clear()
    if pending or len(packets) < 3 or not packets[0].startswith(b"OpusHead") or not packets[1].startswith(b"OpusTags"):
        raise ValueError("incomplete Ogg Opus fixture")
    voice = packets[2:]
    if any(packet_duration_ms(packet) != 20 or len(packet) > 120 for packet in voice):
        raise ValueError("voice fixture requires 20 ms packets at no more than 48 kbps")
    return voice
