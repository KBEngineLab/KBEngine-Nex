kbe_add_server_executable(loginapp KBE_LOGINAPP
    SOURCES
        clientsdk_downloader.cpp
        http_cb_handler.cpp
        loginapp.cpp
        loginapp_interface.cpp
        main.cpp
        profile.cpp
    DEFINITIONS
        USE_PYTHON
    LIBRARIES
        jwsmtp
)
