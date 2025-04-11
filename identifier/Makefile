CUR_DIR = $(shell pwd)
SRC_DIR := ${CUR_DIR}/src
BUILD_DIR := ${CUR_DIR}/build
LLVM_BUILD := /usr/local

NPROC := ${shell nproc}

build_ka_func = \
	(mkdir -p ${2} \
		&& cd ${2} \
		&& PATH=${LLVM_BUILD}/bin:${PATH} \
			LLVM_ROOT_DIR=${LLVM_BUILD}/bin \
			LLVM_LIBRARY_DIRS=${LLVM_BUILD}/lib \
			LLVM_INCLUDE_DIRS=${LLVM_BUILD}/include \
			CC=${LLVM_ROOT_DIR}/clang CXX=${LLVM_ROOT_DIR}/clang++ \
            cmake ${1} \
                -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_CXX_FLAGS_RELEASE="-std=c++14 -fno-rtti -fpic -g" \
		&& make -j${NPROC})

all: identifier

identifier:
	$(call build_ka_func, ${SRC_DIR}, ${BUILD_DIR})
