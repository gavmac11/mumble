"""ctypes adapter for the actual C++ client PacketQueue used in relay experiments."""

import ctypes


class NativePacer:
    def __init__(self, library, rate):
        self.library = ctypes.CDLL(str(library))
        self.library.mumble_pilot_pacer_new.argtypes = [ctypes.c_uint64]
        self.library.mumble_pilot_pacer_new.restype = ctypes.c_void_p
        self.library.mumble_pilot_pacer_free.argtypes = [ctypes.c_void_p]
        self.library.mumble_pilot_pacer_free.restype = None
        self.library.mumble_pilot_pacer_enqueue.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
            ctypes.c_uint, ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_uint64]
        self.library.mumble_pilot_pacer_enqueue.restype = ctypes.c_int
        self.library.mumble_pilot_pacer_take.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
                                                       ctypes.c_void_p, ctypes.c_uint]
        self.library.mumble_pilot_pacer_take.restype = ctypes.c_int
        self.library.mumble_pilot_pacer_delay.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        self.library.mumble_pilot_pacer_delay.restype = ctypes.c_int64
        self.handle = self.library.mumble_pilot_pacer_new(rate)
        if not self.handle:
            raise ValueError("native pacer initialization failed")

    def enqueue(self, packets, keyframe, now):
        data = b"".join(packets)
        sizes = (ctypes.c_uint * len(packets))(*(len(packet) for packet in packets))
        result = self.library.mumble_pilot_pacer_enqueue(self.handle, data, len(data), sizes,
                                                        len(packets), keyframe, now)
        if result < 0:
            raise ValueError("invalid native pacer input")
        return bool(result)

    def take(self, now):
        output = ctypes.create_string_buffer(1024)
        count = self.library.mumble_pilot_pacer_take(self.handle, now, output, len(output))
        if count < 0:
            raise ValueError("native pacer take failed")
        return output.raw[:count] if count else None

    def delay_ns(self, now):
        result = self.library.mumble_pilot_pacer_delay(self.handle, now)
        if result < -1:
            raise ValueError("native pacer delay failed")
        return None if result == -1 else result

    def close(self):
        if self.handle:
            self.library.mumble_pilot_pacer_free(self.handle)
            self.handle = None
