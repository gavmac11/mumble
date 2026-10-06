"""ctypes adapter for the test-only bridge built from pinned Mumble sources."""

import ctypes


class NativeCrypto:
    def __init__(self, library, key, client_nonce, server_nonce):
        if any(len(value) != 16 for value in (key, client_nonce, server_nonce)):
            raise ValueError("CryptSetup requires three 16-byte values")
        self.library = ctypes.CDLL(str(library))
        self.library.mumble_pilot_crypto_new.argtypes = [ctypes.c_void_p] * 3
        self.library.mumble_pilot_crypto_new.restype = ctypes.c_void_p
        self.library.mumble_pilot_crypto_free.argtypes = [ctypes.c_void_p]
        self.library.mumble_pilot_crypto_free.restype = None
        signature = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint,
                     ctypes.c_void_p, ctypes.c_uint]
        for name in ("mumble_pilot_encrypt", "mumble_pilot_decrypt"):
            function = getattr(self.library, name)
            function.argtypes = signature
            function.restype = ctypes.c_int
        self.handle = self.library.mumble_pilot_crypto_new(key, client_nonce, server_nonce)
        if not self.handle:
            raise ValueError("Mumble crypto initialization failed")

    def transform(self, name, data):
        if not self.handle:
            raise ValueError("crypto context is closed")
        output = ctypes.create_string_buffer(1028)
        count = getattr(self.library, name)(self.handle, data, len(data), output, len(output))
        return output.raw[:count] if count >= 0 else None

    def encrypt(self, data):
        result = self.transform("mumble_pilot_encrypt", data)
        if result is None:
            raise ValueError("Mumble encryption failed")
        return result

    def decrypt(self, data):
        return self.transform("mumble_pilot_decrypt", data)

    def close(self):
        if self.handle:
            self.library.mumble_pilot_crypto_free(self.handle)
            self.handle = None
