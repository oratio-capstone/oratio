#!/usr/bin/python3

import socket

local_ip = "0.0.0.0"
local_port = 8989
buffer_size = 1024

nodes = set()


def main():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((local_ip, local_port))

        while True:
            buffer, sender = sock.recvfrom(buffer_size)
            # print(f"received: {buffer}")
            # print(f"from: {sender}")
            # register node
            if sender not in nodes:
                nodes.add(sender)
                print(f"{sender} has joined")

            # received buffer was a heartbeat, shouldn't be broadcast

            if chr(buffer[0]) == "H":
                continue

            for node in nodes:
                if node == sender:
                    continue

                sock.sendto(buffer, node)


if __name__ == "__main__":
    main()
