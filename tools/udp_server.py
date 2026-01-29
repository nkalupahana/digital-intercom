import socket

HOST = "<IP>"
PORT = 9999

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((HOST, PORT))

print(f"Listening for UDP packets on {HOST}:{PORT}...")

while True:
    data = sock.recv(4)  # buffer size
    sample = int.from_bytes(data, byteorder="little")
    print(sample)
