import socket

HOST = "0.0.0.0"
PORT = 9998

OPEN_DOOR = b"D"
LISTEN_ON = b"L"
LISTEN_OFF = b"S"


sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.bind((HOST, PORT))
sock.listen()

print(f"Started TCP Server on {HOST}:{PORT}...")

while True:
    print("Waiting for connection")
    c, addr = sock.accept()
    print("Connected to", addr)
    while True:
        cmd = input().strip().encode("utf-8")
        if len(cmd) != 1:
            print("Invalid len", len(cmd))
            continue
        print("Sending", cmd)
        c.send(cmd)
