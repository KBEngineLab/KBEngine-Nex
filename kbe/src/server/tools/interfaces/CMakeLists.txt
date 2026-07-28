kbe_add_server_executable(interfaces KBE_INTERFACES
    SOURCES
        interfaces_tasks.cpp
        interfaces.cpp
        interfaces_interface.cpp
        main.cpp
        orders.cpp
        profile.cpp
    DEFINITIONS
        USE_PYTHON
        USE_KBE_MYSQL
    LIBRARIES
        jwsmtp
)
