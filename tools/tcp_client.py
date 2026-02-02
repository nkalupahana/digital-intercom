import socket

HOST = "127.0.0.1"
PORT = 9998


while True:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    print(f"Connecting to {HOST}:{PORT}")
    sock.connect((HOST, PORT))
    print(f"Connected to {HOST}:{PORT}")
    data = sock.recv(1024).decode("utf-8")
    print("Recieved", data)
