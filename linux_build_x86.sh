#!/bin/bash

cd out-linux-x86 

    #-DCMAKE_INSTALL_PREFIX=/home/houzh/AI/deplibs/linux-x86/tengine \
cmake  \
    -DPROTOBUF_DIR=/home/houzh/AI/deplibs/linux-x86/protobuf \
    -DBLAS_DIR=/home/houzh/AI/deplibs/linux-x86/openblas \
    -DCMAKE_INSTALL_PREFIX=/home/houzh/AI/deplibs/linux-x86/tengine \
    -DTENGINE_DIR=/home/houzh/AI/deplibs/linux-x86/tengine \
    -DCONFIG_ARCH_BLAS=ON \
    -DCONFIG_ARCH_ARM64=OFF \
    .. 
