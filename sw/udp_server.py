#!/usr/bin/python3

import socket

local_ip = "0.0.0.0"
local_port = 8989
buffer_size = 1024

nodes = {}


def main():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((local_ip, local_port))

        while True:
            try:
                buffer, sender = sock.recvfrom(buffer_size)
            except ConnectionResetError:
                continue

            # erroneous packet
            if len(buffer) < 7:
                continue
            # register node
            node_id = (buffer[1] << 8) | buffer[2]  # extract node_id
            if node_id not in nodes:
                nodes[node_id] = sender
                print(f"{node_id} has joined on {sender}")
            elif nodes[node_id] != sender:
                nodes[node_id] = sender
                print(f"{node_id} has a new ip: {sender}")

            # received packet isnt audio, dont forward it
            if chr(buffer[0]) != "A":
                continue

            for node, node_address in nodes.items():
                if node == node_id:
                    continue

                sock.sendto(buffer, node_address)


if __name__ == "__main__":
    main()
