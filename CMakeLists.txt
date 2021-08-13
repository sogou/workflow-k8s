cmake_minimum_required(VERSION 3.6)

project(
    k8s-service
    VERSION 1.0
    LANGUAGES CXX
)

include(GNUInstallDirs)
string(TIMESTAMP TODAY "%Y%m%d")

set(CMAKE_BUILD_TYPE RelWithDebInfo)
set(CMAKE_SKIP_RPATH TRUE)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_FLAGS "-Wall -Wextra -Wno-unused-parameter")

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/_lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/_lib)
set(INC_DIR ${PROJECT_BINARY_DIR}/_include)

set(INCLUDE_HEADERS
    src/K8sGlobal.h
    src/K8sServiceManager.h
    src/K8sServiceHandler.h
)

add_custom_target(LINK_HEADERS ALL)

macro(makeLink src dest target)
    add_custom_command(
        TARGET ${target} PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${src} ${dest}
        DEPENDS ${dest}
    )
endmacro()

add_custom_command(
    TARGET LINK_HEADERS PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${INC_DIR}/${PROJECT_NAME}
)

foreach(header_file ${INCLUDE_HEADERS})
    string(REPLACE "/" ";" arr ${header_file})
    list(GET arr -1 file_name)
    makeLink(${PROJECT_SOURCE_DIR}/${header_file} ${INC_DIR}/${PROJECT_NAME}/${file_name} LINK_HEADERS)
endforeach()

add_subdirectory(src)

install (
    FILES ${INCLUDE_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}
    COMPONENT devel
)

set(CPACK_GENERATOR "RPM")
set(CPACK_PACKAGE_NAME "k8s-service")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "k8s-service: kubernetes service manager with workflow")
#set(CPACK_PACKAGE_VENDOR "todo")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
# set(CPACK_RPM_PACKAGE_GROUP "")
#set(CPACK_RPM_PACKAGE_URL "http://todo/")
# set(CPACK_RPM_PACKAGE_DESCRIPTION "")
set(CPACK_RPM_PACKAGE_LICENSE "Apache 2.0")
set(CPACK_RPM_PACKAGE_REQUIRES "workflow-devel")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)
set(CPACK_RPM_PACKAGE_RELEASE ${TODAY})

include(CPack)
