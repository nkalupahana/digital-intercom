import socket
import numpy as np
from scipy.io.wavfile import write

HOST = "0.0.0.0"
PORT = 9999
BUFFER_SIZE = 1024
SAMPLE_RATE = 22050

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((HOST, PORT))
samples = []

print(f"Listening for UDP packets on {HOST}:{PORT}...")

# 0 to 2^16
# -2^15-1 to 2^15

try:
    while True:
        data = sock.recv(BUFFER_SIZE)
        for i in range(0, len(data), 2):
            sample = int.from_bytes(data[i:i+2], byteorder="little")
            samples.append(sample)
except KeyboardInterrupt:
    pass


print()
assert len(samples) > 0
samples = np.array(samples, dtype=np.int32)
print(np.min(samples), np.max(samples))
samples -= np.min(samples)
samples = samples * (2**16 // np.max(samples)) - 2**15
print(np.min(samples), np.max(samples))
samples = samples.astype(np.int16)

print("Writing", len(samples), len(samples) / SAMPLE_RATE, "seconds")
write("test.wav", SAMPLE_RATE, samples)
