#!/bin/bash

# Create disk1.img (1MB, zeroed)
dd if=/dev/zero of=disk1.img bs=1M count=1

# Create disk2.img (1MB, zeroed)
dd if=/dev/zero of=disk2.img bs=1M count=1

# Make both files readable
chmod 644 disk1.img disk2.img
