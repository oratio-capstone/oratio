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
            buffer, sender = sock.recvfrom(buffer_size)
            # print(f"received: {buffer}")
            # print(f"from: {sender}")

            # register node
            # TODO: correct based on Fayza's implementation
            node_id = (buffer[1] << 8) | buffer[2]  # extract node_id
            if node_id not in nodes:
                nodes[node_id] = sender
                print(f"{node_id} has joined on {sender}")

            # received buffer was a heartbeat, shouldn't be broadcast
            if chr(buffer[0]) == "H":
                continue

            for node, node_address in nodes.items():
                if node == node_id:
                    continue

                sock.sendto(buffer, node_address)


if __name__ == "__main__":
    main()
