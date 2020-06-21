#!/bin/bash
cd build
cmake .. -DBUILD_OPENSSL=TRUE -DBUILD_OPENSSL_PLATFORM=linux-generic32 -DBUILD_LIBSRTP_HOST_PLATFORM=x86_64-unknown-linux-gnu -DBUILD_LIBSRTP_DESTINATION_PLATFORM=arm-unknown-linux-uclibcgnueabihf
make
cd ..
