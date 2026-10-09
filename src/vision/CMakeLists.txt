add_library(tobii-vision
    preprocess.cpp
    anchors.cpp
    face_detector.cpp
    face_landmarks.cpp
    pose.cpp
    tracker.cpp
)
target_include_directories(tobii-vision
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PUBLIC ${ONNXRUNTIME_INCLUDE_DIR}
)
target_link_libraries(tobii-vision
    PUBLIC tobii-opencv PkgConfig::EIGEN
    PUBLIC ${ONNXRUNTIME_LIB}
)
target_compile_definitions(tobii-vision PRIVATE TOBII_MODEL_DIR="${TOBII_MODEL_DIR}")
