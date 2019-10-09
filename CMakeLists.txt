cmake_policy(SET CMP0048 NEW)
project(st VERSION 0.0.0.0 LANGUAGES C)
cmake_minimum_required(VERSION 3.15.2)

find_package(Freetype REQUIRED)
find_package(Fontconfig REQUIRED)
find_package(X11 REQUIRED)
#find_package(FLTK REQUIRED NO_MODULE)

set(SOURCES ${PROJECT_SOURCE_DIR}/st.c ${PROJECT_SOURCE_DIR}/x.c config.h st.h win.h)
add_executable(${PROJECT_NAME} ${SOURCES})
target_compile_definitions(${PROJECT_NAME} PUBLIC VERSION="customVersion")
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
target_include_directories(${PROJECT_NAME} PUBLIC ${PROJECT_SOURCE_DIR} PRIVATE ${X11_INCLUDE_DIR} ${FONTCONFIG_INCLUDE_DIR} )
target_link_libraries(${PROJECT_NAME} Freetype::Freetype ${X11_LIBRARIES} ${FONTCONFIG_LIBRARIES} fontconfig m -lXft -lutil)

#install directives


