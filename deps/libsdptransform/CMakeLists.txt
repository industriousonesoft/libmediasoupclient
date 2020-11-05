cmake_minimum_required(VERSION 3.0)

project(sdptransform VERSION 1.2.8)

# For CMake >= 3.1.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# For CMake < 3.1.
add_compile_options(-std=c++11)

#Project Configuation
#确保nlohmann::json的版本一致
set(LIBJSON_INCLUDE_PATH "" CACHE STRING "nlohmann json include path")

if(NOT LIBJSON_INCLUDE_PATH)
	message(FATAL_ERROR "LIBJSON_INCLUDE_PATH not provided")
endif()

subdirs(test readme-helper)

include_directories(${sdptransform_SOURCE_DIR}/include)

set(
	SOURCE_FILES
	src/grammar.cpp
	src/parser.cpp
	src/writer.cpp
)

set(
	HEADER_FILES
	include/sdptransform.hpp
	#deps/json/include/nlohmann/json.hpp #改为LIBJSON_INCLUDE_PATH
)

add_library(sdptransform STATIC ${SOURCE_FILES} ${HEADER_FILES})

target_include_directories(${PROJECT_NAME} PUBLIC
 	"${LIBJSON_INCLUDE_PATH}"
 )

install(TARGETS sdptransform DESTINATION lib)
install(FILES ${HEADER_FILES} DESTINATION include/sdptransform)
