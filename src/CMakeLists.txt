cmake_minimum_required(VERSION 3.6)

find_package(OpenSSL REQUIRED)

set(SRC
    K8sHttpTask.cc
    K8sServiceManager.cc
    K8sSSLWrapper.cc
    K8sServiceHandler.cc
)

if(JSONLIB STREQUAL "jsoncpp")
    set(SRC ${SRC} K8sJsoncppParser.cc)
else()
    set(SRC ${SRC} K8sJsoncppParser.cc)
endif()

add_library(${PROJECT_NAME} STATIC ${SRC})

install(
    TARGETS ${PROJECT_NAME}
    ARCHIVE
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT devel
)
