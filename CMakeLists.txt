cmake_minimum_required(VERSION 3.20)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)


# create project
project(USD_Importer CXX C)

include(GetPrerequisites)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_BINARY_DIR})
list(APPEND CMAKE_PREFIX_PATH ${CMAKE_BINARY_DIR})
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR})

if(MSVC)
	# Make Visual Studio report accurate C++ version
	# See: https://devblogs.microsoft.com/cppblog/msvc-now-correctly-reports-__cplusplus/
  # /Wall emits warnings about the C++ standard library
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /GS- /Zc:__cplusplus")

	add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
	add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
else()
	# Ignore Visual Studio pragma regions
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unknown-pragmas")
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} --coverage -Og")
endif(MSVC)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# add harfang sdk
set(HG_SRC_DIR "../harfang" CACHE PATH "Path to the Harfang source directory")
set(USD_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "Path to the USD build directory")

set(HG_BUILD_ASSIMP_CONVERTER OFF)
set(HG_BUILD_FBX_CONVERTER OFF)
set(HG_BUILD_GLTF_EXPORTER OFF)
set(HG_BUILD_GLTF_IMPORTER OFF)
set(HG_BUILD_USD_IMPORTER OFF)
set(HG_BUILD_SPHERICAL_HARMONICS_EXTRACTOR OFF)
set(HG_BUILD_ASSETC OFF)
set(HG_ENABLE_OPENVR_API OFF)
set(HG_ENABLE_OPENXR_API OFF)
set(HG_ENABLE_RECAST_DETOUR_API OFF)
set(HG_ENABLE_BULLET3_SCENE_PHYSICS OFF)
set(HG_BUILD_CPP_SDK OFF)
set(HG_BUILD_HG_PYTHON OFF)
set(HG_BUILD_TESTS OFF)	
add_subdirectory(${HG_SRC_DIR} hg EXCLUDE_FROM_ALL)	
include_directories(${HG_SRC_DIR}/extern)

# create USD_Importer project
add_executable(USD_Importer usd_importer.cpp)
target_link_libraries(USD_Importer PUBLIC engine)
target_include_directories(USD_Importer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

SET_TARGET_PROPERTIES(USD_Importer PROPERTIES COMPILE_FLAGS_RELEASE "/GL")

if(WIN32)
	SET_TARGET_PROPERTIES(USD_Importer PROPERTIES LINK_FLAGS_RELEASE "/LTCG")
	add_compile_options(/permissive- /std:c++latest)
	set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT USD_Importer)
	set_target_properties(USD_Importer PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/USD_Importer")
endif()

# modify where cmake install directory
set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "Cmake prefix" FORCE)

# install binary, runtime dependencies
install(TARGETS USD_Importer ARCHIVE DESTINATION USD_Importer RUNTIME DESTINATION USD_Importer LIBRARY DESTINATION USD_Importer COMPONENT USD_Importer)

if (WIN32)
    # ".lib" on Windows
    set(USD_LIB_SUFFIX ${CMAKE_STATIC_LIBRARY_SUFFIX}
        CACHE STRING "Extension of USD libraries")
else ()
    # ".so" on Linux, ".dylib" on MacOS
    set(USD_LIB_SUFFIX ${CMAKE_SHARED_LIBRARY_SUFFIX}
        CACHE STRING "Extension of USD libraries")
endif ()

foreach(TYPE Release)# Debug)
	# look for usd project
	set(pxr_DIR "${USD_SDK_DIR}/USD_build_${TYPE}")

#	find_package(Boost PATHS pxr_DIR) # headers only

	target_link_libraries(USD_Importer PUBLIC  
								$<$<CONFIG:${TYPE}>:${pxr_DIR}/lib/tbb_debug${USD_LIB_SUFFIX}> 
								$<$<CONFIG:${TYPE}>:${pxr_DIR}/lib/tbb${USD_LIB_SUFFIX}> 
								$<$<CONFIG:${TYPE}>:${pxr_DIR}/lib/usd_usd_ms${USD_LIB_SUFFIX}>
								$<$<CONFIG:${TYPE}>:${pxr_DIR}/lib/Ptex${USD_LIB_SUFFIX}>)

	target_include_directories(USD_Importer PUBLIC	$<$<CONFIG:${TYPE}>:${pxr_DIR}/include> 
													$<$<CONFIG:${TYPE}>:${pxr_DIR}/include/boost-1_78>)
												#	$<$<CONFIG:${TYPE}>:${Boost_INCLUDE_DIRS}>)

	install(DIRECTORY "${pxr_DIR}/lib/"
 			CONFIGURATIONS ${TYPE}
 	 		DESTINATION USD_Importer COMPONENT USD_Importer
 			FILES_MATCHING PATTERN "*.dll")
	install(DIRECTORY "${pxr_DIR}/lib/usd"
 			CONFIGURATIONS ${TYPE}
 			DESTINATION USD_Importer COMPONENT USD_Importer)
	install(DIRECTORY "${pxr_DIR}/bin/"
 			CONFIGURATIONS ${TYPE}
 			DESTINATION USD_Importer COMPONENT USD_Importer
 			FILES_MATCHING PATTERN "*.dll")
endforeach()

# usd importer install
add_custom_target(usd_importer_copy ALL
	COMMAND ${CMAKE_COMMAND} -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX} -DBUILD_TYPE=$<CONFIG> -P ${CMAKE_BINARY_DIR}/cmake_install.cmake
	DEPENDS USD_Importer
)
set_target_properties(usd_importer_copy PROPERTIES FOLDER "CMakePredefinedTargets")

