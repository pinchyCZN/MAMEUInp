#!/bin/sh
# Fallback for a box with a compiler but no make. Identical result to "make".
#
#     sh build.sh
#
# See the Makefile for what NETPLAY_STANDALONE does and why it is required.
set -e

CXX=${CXX:-g++}

"$CXX" -O2 -std=c++14 -Wall -I. -DNETPLAY_STANDALONE \
    -o netplay_server \
    netplay_server.cpp \
    netplay/netplay_packet.cpp

echo "built ./netplay_server"
