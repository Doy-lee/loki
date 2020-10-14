#!/bin/bash
#
#  -D BOOST_ROOT=/opt/android/boost_1_58_0

set -e

orig_path=$PATH
base_dir=`pwd`

build_type=release # or debug
android_api=21
archs=(arm arm64 x86 x86_64)

orig_cxx_flags=$CXXFLAGS
for arch in ${archs[@]}; do
    extra_cmake_flags=""
    extra_cxx_flags=""

    case ${arch} in
        "arm")
          target_host=arm-linux-androideabi
          xtarget_host=armv7a-linux-androideabi
          xarch=armv7-a
          sixtyfour=OFF
          extra_cmake_flags="-D NO_AES=true"
          ;;
        "arm64")
          target_host=aarch64-linux-android
          xtarget_host=${target_host}
          xarch="armv8-a+crypto"
          sixtyfour=ON
          ;;
        "x86")
          target_host=i686-linux-android
          xtarget_host=${target_host}
          xarch="i686"
          extra_c_flags="-maes"
          extra_cxx_flags="-maes"
          ;;
        "x86_64")
          target_host=x86_64-linux-android
          xtarget_host=${target_host}
          xarch="x86-64"
          sixtyfour=ON
          extra_c_flags="-maes"
          extra_cxx_flags="-maes"
          ;;
        *)
          exit 16
          ;;
    esac

    OUTPUT_DIR=$base_dir/build/$build_type.$arch
    mkdir -p $OUTPUT_DIR
    cd $OUTPUT_DIR

# warning: unknown warning option '-Wduplicated-cond'; did you mean '-Wduplicate-enum'? [-Wunknown-warning-option]
# warning: unknown warning option '-Wduplicated-branches' [-Wunknown-warning-option]
# warning: unknown warning option '-Wlogical-op'; did you mean '-Wlong-long'? [-Wunknown-warning-option]
# warning: unknown warning option '-Wrestrict' [-Wunknown-warning-option]
# warning: unknown warning option '-Wjump-misses-init' [-Wunknown-warning-option]
# warning: unknown warning option '-Wmisleading-indentation'; did you mean '-Wbinding-in-condition'? [-Wunknown-warning-option]

    # OpenSSL needs ANDROID_NDK_HOME Set and path to toolchain in bin
    android_toolchain=/opt/android/ndk/toolchains/llvm/prebuilt/linux-x86_64
    export PATH=${android_toolchain}/bin:${PATH}
    export ANDROID_NDK_HOME="/opt/android/ndk"

    export PKG_CONFIG_PATH="/opt/android/build/libsodium/$arch/lib/pkgconfig:/opt/android/build/libzmq/$arch/lib/pkgconfig:/opt/android/build/$arch/lib/pkgconfig"
    # CC=clang \
    # CXX=clang++ \

    AR=${android_toolchain}/bin/${target_host}-ar \
    AS=${android_toolchain}/bin/${target_host}-as \
    CC=${android_toolchain}/bin/${xtarget_host}${android_api}-clang \
    CXX=${android_toolchain}/bin/${xtarget_host}${android_api}-clang++ \
    LD=${android_toolchain}/bin/${target_host}-ld \
    RANLIB=${android_toolchain}/bin/${target_host}-ranlib \
    STRIP=${android_toolchain}/bin/${target_host}-strip \
    cmake \
      -D BUILD_GUI_DEPS=1 \
      -D BUILD_STATIC_DEPS=OFF \
      -D BUILD_TESTS=OFF \
      -D ARCH="$xarch" \
      -D STATIC=ON \
      -D BUILD_64=$sixtyfour \
      -D CMAKE_BUILD_TYPE=$build_type \
      -D CMAKE_CXX_FLAGS="-D__ANDROID_API__=$android_api -isystem /opt/android/build/libsodium/$arch/include/ ${extra_cxx_flags}" \
      -D CMAKE_C_FLAGS="${extra_c_flags}" \
      -D ANDROID=true \
      -D BUILD_TAG="android" \
      -D BOOST_ROOT=/opt/android/build/$arch \
      -D Boost_DEBUG=ON \
      -D BOOST_IGNORE_SYSTEM_PATHS=ON \
      -D CMAKE_POSITION_INDEPENDENT_CODE:BOOL=true \
       $extra_cmake_flags \
      ../..

    AR=${android_toolchain}/bin/${target_host}-ar \
    AS=${android_toolchain}/bin/${target_host}-as \
    CC=${android_toolchain}/bin/${xtarget_host}${android_api}-clang \
    CXX=${android_toolchain}/bin/${xtarget_host}${android_api}-clang++ \
    LD=${android_toolchain}/bin/${target_host}-ld \
    RANLIB=${android_toolchain}/bin/${target_host}-ranlib \
    STRIP=${android_toolchain}/bin/${target_host}-strip \
    make VERBOSE=1 -j24 wallet_api

    DEST_LIB=/opt/android/build/monero/$arch/lib
    mkdir -p ${DEST_LIB} && shopt -s globstar && cp -f --backup=numbered **/*.a ${DEST_LIB}

    DEST_INCLUDE=/opt/android/build/monero/include
    mkdir -p ${DEST_INCLUDE}
    cp -fa ../../src/wallet/api/wallet2_api.h ${DEST_INCLUDE}

    cd $base_dir
done
exit 0

