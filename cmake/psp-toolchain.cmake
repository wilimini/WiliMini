if(NOT DEFINED ENV{PSPDEV})
    message(FATAL_ERROR "The PSPDEV environment variable is not set")
endif()

include("$ENV{PSPDEV}/psp/share/pspdev.cmake")
