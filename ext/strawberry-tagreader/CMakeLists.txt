cmake_minimum_required(VERSION 3.7)

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR})

set(SOURCES main.cpp tagreaderworker.cpp)
set(HEADERS tagreaderworker.h)

qt_wrap_cpp(MOC ${HEADERS})

link_directories(${GLIB_LIBRARY_DIRS})

if(USE_TAGLIB AND TAGLIB_FOUND)
  link_directories(${TAGLIB_LIBRARY_DIRS})
endif()

if(USE_TAGPARSER AND TAGPARSER_FOUND)
  link_directories(${TAGPARSER_LIBRARY_DIRS})
endif()

add_executable(strawberry-tagreader ${SOURCES} ${MOC} ${QRC})

target_include_directories(strawberry-tagreader SYSTEM PRIVATE
  ${GLIB_INCLUDE_DIRS}
  ${PROTOBUF_INCLUDE_DIRS}
)

target_include_directories(strawberry-tagreader PRIVATE
  ${CMAKE_SOURCE_DIR}/ext/libstrawberry-common
  ${CMAKE_SOURCE_DIR}/ext/libstrawberry-tagreader
  ${CMAKE_BINARY_DIR}/ext/libstrawberry-tagreader
  ${CMAKE_BINARY_DIR}/src
)

target_link_libraries(strawberry-tagreader PRIVATE
  ${GLIB_LIBRARIES}
  ${QtCore_LIBRARIES}
  ${QtNetwork_LIBRARIES}
  libstrawberry-common
  libstrawberry-tagreader
)

if(USE_TAGLIB AND TAGLIB_FOUND)
  target_include_directories(strawberry-tagreader SYSTEM PRIVATE ${TAGLIB_INCLUDE_DIRS})
  target_link_libraries(strawberry-tagreader PRIVATE ${TAGLIB_LIBRARIES})
endif()

if(USE_TAGPARSER AND TAGPARSER_FOUND)
  target_include_directories(strawberry-tagreader SYSTEM PRIVATE ${TAGPARSER_INCLUDE_DIRS})
  target_link_libraries(strawberry-tagreader PRIVATE ${TAGPARSER_LIBRARIES})
endif()

if(FREEBSD)
  target_link_libraries(strawberry-tagreader PRIVATE execinfo)
endif()

if(APPLE)
  target_link_libraries(strawberry-tagreader PRIVATE /System/Library/Frameworks/Foundation.framework)
endif()

if(APPLE)
  install(TARGETS strawberry-tagreader DESTINATION ${CMAKE_BINARY_DIR}/strawberry.app/Contents/PlugIns)
else()
  install(TARGETS strawberry-tagreader RUNTIME DESTINATION bin)
endif()
