import socket

sock = socket.socket(socket.AF_UNIX);
print(socket.socket(socket.AF_UNIX, fileno=sock.fileno()))
