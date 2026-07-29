add_library(math STATIC
    ../dependencies/g3dlite/AABox.cpp
    ../dependencies/g3dlite/Box.cpp
    ../dependencies/g3dlite/Crypto.cpp
    ../dependencies/g3dlite/format.cpp
    math.cpp
    ../dependencies/g3dlite/Matrix3.cpp
    ../dependencies/g3dlite/Plane.cpp
    ../dependencies/g3dlite/System.cpp
    ../dependencies/g3dlite/Triangle.cpp
    ../dependencies/g3dlite/Vector3.cpp
    ../dependencies/g3dlite/Vector4.cpp
)

kbe_configure_target(math)
target_link_libraries(math PUBLIC common)
