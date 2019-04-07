cmake_minimum_required (VERSION 2.8)
project(wrapper)

if(NOT INSTALL_DIR)
set( INSTALL_DIR ${TENGINE_DIR}/install/ )
endif()

set( WRAPPER_LIBS tengine)
aux_source_directory(caffe CAFFE_SRC)
aux_source_directory(tensorflow TF_SRC)

message("----generate caffe.pb.h----${CMAKE_CURRENT_SOURCE_DIR}")
set(caffe_proto_cc ${CMAKE_CURRENT_SOURCE_DIR}/caffe/caffe.pb.cc)
set(caffe_dir ${CMAKE_CURRENT_SOURCE_DIR}/caffe)
ADD_CUSTOM_COMMAND(OUTPUT ${caffe_proto_cc}
                   COMMAND protoc --cpp_out=${caffe_dir} --proto_path=${caffe_dir} ${caffe_dir}/caffe.proto
                   )
set(CAFFE_SRC ${CAFFE_SRC} ${caffe_proto_cc})


#include
include_directories(${INSTALL_DIR}/include
                    ${CMAKE_CURRENT_SOURCE_DIR}/include
                    ${CMAKE_CURRENT_SOURCE_DIR}/caffe
		    )

if(ANDROID)
  if(("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv7-a"))
   include_directories(${ANDROID_NDK}/sysroot/usr/include/arm-linux-androideabi)
  elseif(("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "aarch64"))
   include_directories(${ANDROID_NDK}/sysroot/usr/include/aarch64-linux-android)
  endif()
endif()

set(CMAKE_CXX_FLAGS "-std=c++11 -O3 -Wall")

#lib
add_library(wrapper ${CAFFE_SRC} ${TF_SRC})
link_directories(${INSTALL_DIR}/lib)


#exe
target_link_libraries(wrapper ${WRAPPER_LIBS})
install(TARGETS  wrapper  DESTINATION lib)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/caffe/caffe.pb.h DESTINATION wrapper_include)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/caffe DESTINATION wrapper_include)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/tensorflow DESTINATION wrapper_include)
