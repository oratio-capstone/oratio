#!/usr/bin/python3

import socket

local_ip = "0.0.0.0"
local_port = 8989
buffer_size = 1024


def main():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((local_ip, local_port))

        buffer = []
        while True:
            buffer, sender = sock.recvfrom(buffer_size)
            print(f"received: {buffer}\n from: {sender}")
            sock.sendto(buffer, sender)


if __name__ == "__main__":
    main()
