#!/bin/bash
set -euo pipefail

OPENCV_VERSION=4.13.0
OPENCV_SHA256=1d40ca017ea51c533cf9fd5cbde5b5fe7ae248291ddf2af99d4c17cf8e13017d
ORT_VERSION=1.22.0
ORT_SHA256=8344d55f93d5bc5021ce342db50f62079daf39aaafb5d311a451846228be49b3

PREFIX="${1:?usage: build-deps.sh PREFIX}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fetch() {
    local url="$1" sha="$2" out="$3"
    curl -fsSL --retry 3 -o "$out" "$url"
    echo "$sha  $out" | sha256sum -c --quiet - || { echo "checksum mismatch: $url" >&2; exit 1; }
}

echo "== onnxruntime $ORT_VERSION"
fetch "https://github.com/microsoft/onnxruntime/releases/download/v$ORT_VERSION/onnxruntime-linux-x64-$ORT_VERSION.tgz" \
      "$ORT_SHA256" "$WORK/ort.tgz"
mkdir -p "$PREFIX/onnxruntime"
tar -xzf "$WORK/ort.tgz" -C "$PREFIX/onnxruntime" --strip-components=1

echo "== opencv $OPENCV_VERSION (static: core imgproc imgcodecs, JPEG and PNG)"
fetch "https://github.com/opencv/opencv/archive/refs/tags/$OPENCV_VERSION.tar.gz" \
      "$OPENCV_SHA256" "$WORK/opencv.tar.gz"
tar -xzf "$WORK/opencv.tar.gz" -C "$WORK"
cmake -S "$WORK/opencv-$OPENCV_VERSION" -B "$WORK/opencv-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX/opencv" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_LIST=core,imgproc,imgcodecs \
    -DBUILD_ZLIB=ON -DBUILD_JPEG=ON -DWITH_JPEG=ON -DBUILD_PNG=ON -DWITH_PNG=ON \
    -DWITH_TIFF=OFF -DWITH_WEBP=OFF -DWITH_OPENJPEG=OFF \
    -DWITH_JASPER=OFF -DWITH_OPENEXR=OFF -DWITH_AVIF=OFF -DWITH_SPNG=OFF \
    -DWITH_IMGCODEC_HDR=OFF -DWITH_IMGCODEC_SUNRASTER=OFF \
    -DWITH_IMGCODEC_PXM=OFF -DWITH_IMGCODEC_PFM=OFF -DWITH_IMGCODEC_GIF=OFF \
    -DWITH_IPP=OFF -DWITH_ITT=OFF -DWITH_OPENCL=OFF -DWITH_OPENCLAMDFFT=OFF \
    -DWITH_OPENCLAMDBLAS=OFF -DWITH_VA=OFF -DWITH_VA_INTEL=OFF -DWITH_CUDA=OFF \
    -DWITH_TBB=OFF -DWITH_OPENMP=OFF -DWITH_EIGEN=OFF -DWITH_LAPACK=OFF \
    -DWITH_PROTOBUF=OFF -DWITH_ADE=OFF -DWITH_GTK=OFF -DWITH_QT=OFF \
    -DWITH_FFMPEG=OFF -DWITH_GSTREAMER=OFF -DWITH_V4L=OFF -DWITH_1394=OFF \
    -DWITH_GDAL=OFF -DWITH_GDCM=OFF -DWITH_OPENVX=OFF -DWITH_VTK=OFF \
    -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF \
    -DBUILD_DOCS=OFF -DBUILD_opencv_apps=OFF -DBUILD_JAVA=OFF \
    -DBUILD_opencv_python3=OFF -DOPENCV_GENERATE_PKGCONFIG=OFF \
    -DCV_TRACE=OFF -DOPENCV_ENABLE_NONFREE=OFF >/dev/null
cmake --build "$WORK/opencv-build" -j"$(nproc)"
cmake --install "$WORK/opencv-build" >/dev/null

cat > "$PREFIX/deps.cmake" <<EOF
set(OpenCV_DIR "$PREFIX/opencv/lib/cmake/opencv4" CACHE PATH "")
set(OpenCV_STATIC ON CACHE BOOL "")
set(ONNXRUNTIME_ROOT "$PREFIX/onnxruntime" CACHE PATH "")
set(TOBII_BUNDLE_ONNXRUNTIME ON CACHE BOOL "")
EOF
echo "== deps ready in $PREFIX"
