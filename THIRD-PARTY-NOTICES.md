# Third-party notices

tobii-faithful is licensed under the Apache License 2.0 (`LICENSE`). It
includes, or is built from, the third-party material below. Each entry says what
is used, where it ends up, and under which terms.

## MediaPipe

Copyright The MediaPipe Authors. Licensed under the Apache License 2.0; the
licence text is `LICENSE`. Source: <https://github.com/google-ai-edge/mediapipe>.

- **Face models**: `face_detector.onnx` and `face_landmarks_detector.onnx`,
  installed under `/usr/share/tobii-faithful/models/`. Converted from
  MediaPipe's `face_landmarker.task` model bundle: the TFLite models were
  converted to ONNX and from float16 to float32. Weights are otherwise
  unchanged.
- **Face geometry data**: `include/tobii/face_geometry_data.hpp`, compiled into
  `tobiid`. The canonical face mesh and Procrustes weights
  extracted from the same bundle's geometry pipeline metadata.
- **SSD anchor generation**: `src/vision/anchors.cpp` and
  `include/tobii/vision/anchors.hpp`, compiled into `tobiid`. Based on MediaPipe's
  SSD anchors calculator; modified.

The face pipeline in `src/vision` follows MediaPipe's published algorithms
for face detection decoding, weighted non-maximum suppression, landmark
region of interest and the face geometry solve, so that its output matches
MediaPipe's.

## Dear ImGui

Copyright (c) 2014-2024 Omar Cornut. Licensed under the MIT licence; the
licence text is `third_party/imgui/LICENSE.txt`. Source:
<https://github.com/ocornut/imgui>.

Version 1.91.5, compiled into `tobii-manager`. The vendored source is in
`third_party/imgui` with its notices intact.

## Eigen

Licensed under the Mozilla Public License 2.0; the licence text is
`licenses/MPL-2.0.txt`. Source: <https://gitlab.com/libeigen/eigen>.

Eigen is a header-only library. Its unmodified headers (the Dense and SVD
modules) are compiled into `tobiid`, built with
`EIGEN_MPL2_ONLY` so that only MPL-2.0 code is used. The Source Code Form of
Eigen is available from the URL above, and in your distribution's
`eigen3-devel` package.

## OpenCV

Copyright the OpenCV authors. Licensed under the Apache License 2.0; the licence
text is `licenses/opencv-LICENSE.txt`. Source: <https://github.com/opencv/opencv>.

Version 4.13.0, built unmodified as static libraries (the core, imgproc and
imgcodecs modules only) and linked into `tobiid` and `tobii-manager`. The build
is `release/deps/build-deps.sh` in the source tree (`packaging/build-deps.sh` in
the published one). It includes these components, from OpenCV's own source
tree, also unmodified:

- **zlib**: Copyright (C) 1995-2022 Jean-loup Gailly and Mark Adler. zlib
  licence; `licenses/zlib-LICENSE.txt`.
- **libjpeg-turbo**: the IJG licence, the Modified (3-clause) BSD licence and the
  zlib licence; `licenses/libjpeg-turbo-LICENSE.md` and
  `licenses/libjpeg-turbo-README.ijg`. This software is based in part on the
  work of the Independent JPEG Group.
- **libpng**: the PNG Reference Library License version 2;
  `licenses/libpng-LICENSE.txt`.
- **Berkeley SoftFloat**: Copyright 2011-2017 The Regents of the University of
  California. 3-clause BSD licence; `licenses/SoftFloat-COPYING.txt`.

## ONNX Runtime

Copyright (c) Microsoft Corporation. Licensed under the MIT licence; the licence
text is `licenses/onnxruntime-LICENSE.txt`, and the notices for the components
it contains are `licenses/onnxruntime-ThirdPartyNotices.txt`. Source:
<https://github.com/microsoft/onnxruntime>.

Version 1.22.0, Microsoft's unmodified prebuilt Linux x64 release. Its library
is installed as `libonnxruntime.so.1` under `/usr/lib64/tobii-faithful/` (`/usr/lib/tobii-faithful/` on Arch), used
only by tobii-faithful's own programs.

## mingw-w64 runtime

Copyright (c) 2009 - 2013 by the mingw-w64 project. Licensed under the Zope
Public License 2.1, with parts in the public domain; the licence texts are
`licenses/mingw-w64-runtime.txt` and
`licenses/mingw-w64-runtime-public-domain.txt`. Source:
<https://www.mingw-w64.org/>.

The mingw-w64 C runtime start-up code is statically linked into
`tobii-sesp-bridge.exe`, installed under `/usr/lib64/tobii-faithful/` (`/usr/lib/tobii-faithful/` on Arch). That
executable also contains GCC runtime support code, which is covered by the GCC
Runtime Library Exception and places no conditions on it.

## Libraries used from your system

These are not distributed with tobii-faithful. The programs use your
distribution's packages at run time, and each package carries its own licence:
libusb, GLFW, OpenSSL, OpenGL, systemd and the C/C++ runtime.
