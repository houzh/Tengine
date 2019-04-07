#!/bin/bash

ANDROID_NDK=/opt/android-ndk-r17c
if [ $# == 0 ]; then
  ACL_CONFIG="-DCONFIG_ARCH_BLAS=ON "
  echo "NO PARAMS use BLAS as default"
fi

for i in $*      #在$*中遍历参数，此时每个参数都是独立的，会遍历$#次
do
   if [ $i == 'acl' ]; then
           ACL_CONFIG+="-DCONFIG_ACL_GPU=ON "
   fi

   if [ $i == 'blas' ]; then
	   ACL_CONFIG+="-DCONFIG_ARCH_BLAS=ON "
   fi

   if [ $i == 'Release' ]; then
           ACL_CONFIG+='-DCMAKE_BUILD_TYPE="Release"'
   fi
done

echo $ACL_CONFIG
ARCH_TYPE=ARMv7
 
#-DACL_ROOT=$ACL_ROOT ACL is used on in 64bit CPU
cd out-android-armv7
 
cmake -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
     -DCMAKE_INSTALL_PREFIX=/home/houzh/AI/deplibs/android-armv7/tengine \
     -DPROTOBUF_DIR=/home/houzh/AI/deplibs/android-armv7/protobuf\
     -DBLAS_DIR=/home/houzh/AI/deplibs/android-armv7/openblas \
     -DACL_ROOT=/home/houzh/AI/deplibs/android-armv7/computelibrary \
     -DANDROID_PLATFORM=android-26 \
     -DANDROID_ABI="armeabi-v7a" \
     -DANDROID_STL=c++_shared \
     -DANDROID_ARM_NEON=ON \
     ${ACL_CONFIG} \
     ..

#ACL_ROOT:/home/houzh/AI/deplibs/android-armv7/ComputeLibrary

