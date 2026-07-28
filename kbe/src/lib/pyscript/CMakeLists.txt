add_library(pyscript STATIC
    copy.cpp
    map.cpp
    math.cpp
    pickler.cpp
    pyurl.cpp
    py_gc.cpp
    py_memorystream.cpp
    pyprofile.cpp
    pyprofile_handler.cpp
    pystruct.cpp
    pywatcher.cpp
    py_platform.cpp
    py_compression.cpp
    script.cpp
    scriptobject.cpp
    scriptstderr.cpp
    scriptstdout.cpp
    scriptstdouterr.cpp
    scriptstdouterrhook.cpp
    sequence.cpp
    vector2.cpp
    vector3.cpp
    vector4.cpp
)

kbe_configure_target(pyscript)
target_link_libraries(pyscript PUBLIC common helper math resmgr thread KBE::PythonRuntime)
