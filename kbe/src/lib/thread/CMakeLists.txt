add_library(thread STATIC
    concurrency.cpp
    threadpool.cpp
)

kbe_configure_target(thread)
target_link_libraries(thread PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
