cmake_minimum_required(VERSION 3.9)
project(OptCuts)

set(OPTCUTS_EXTERNAL "${CMAKE_CURRENT_SOURCE_DIR}/ext")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

if(MSVC)
  add_definitions(-DNOMINMAX)
  add_definitions(-DM_PI=3.14159265358979323846264338327950288)
  add_definitions(-DM_PI_2=M_PI*2)
  add_definitions(-D__DBL_MAX__=1.7976931348623158e+308)
endif()

include(${PROJECT_NAME}Dependencies)

# libigl
option(LIBIGL_USE_STATIC_LIBRARY "Use libigl as static library" OFF)
option(LIBIGL_WITH_ANTTWEAKBAR       "Use AntTweakBar"    OFF)
option(LIBIGL_WITH_CGAL              "Use CGAL"           OFF)
option(LIBIGL_WITH_COMISO            "Use CoMiso"         OFF)
option(LIBIGL_WITH_CORK              "Use Cork"           OFF)
option(LIBIGL_WITH_EMBREE            "Use Embree"         OFF)
option(LIBIGL_WITH_LIM               "Use LIM"            OFF)
option(LIBIGL_WITH_MATLAB            "Use Matlab"         OFF)
option(LIBIGL_WITH_MOSEK             "Use MOSEK"          OFF)
option(LIBIGL_WITH_OPENGL            "Use OpenGL"         ON)
option(LIBIGL_WITH_OPENGL_GLFW       "Use GLFW"           ON)
option(LIBIGL_WITH_OPENGL_GLFW_IMGUI "Use ImGui"          OFF)
option(LIBIGL_WITH_PNG               "Use PNG"            ON)
option(LIBIGL_WITH_PYTHON            "Use Python"         OFF)
option(LIBIGL_WITH_TETGEN            "Use Tetgen"         OFF)
option(LIBIGL_WITH_TRIANGLE          "Use Triangle"       ON)
option(LIBIGL_WITH_VIEWER            "Use OpenGL viewer"  ON)
option(LIBIGL_WITH_XML               "Use XML"            OFF)

find_package(LIBIGL REQUIRED QUIET)


# Intel TBB
#include(ext/tbb/cmake/TBBGet.cmake)
#tbb_get(TBB_ROOT tbb_root CONFIG_DIR TBB_DIR)
#find_package(TBB REQUIRED tbb)


file(GLOB SRCFILES
  # src/main.cpp
  src/main_fork.cpp
  src/Optimizer.cpp
  src/TriMesh.cpp
  src/Scaffold.cpp

  src/Energy/Energy.cpp
  src/Energy/ARAPEnergy.cpp
  src/Energy/SymDirichletEnergy.cpp
  
  src/Utils/IglUtils.cpp
  
  src/LinSysSolver/EigenLibSolver.cpp
)

include_directories(
  src
  src/Energy
  src/Utils
  src/LinSysSolver
  # src/UnitTest # Diagnostic.hpp MeshProcessing.hpp
)

add_executable(${PROJECT_NAME}_bin ${SRCFILES})
if(MSVC)
  target_compile_options(${PROJECT_NAME}_bin PRIVATE /wd4819)
endif()

target_link_libraries(${PROJECT_NAME}_bin igl::core igl::opengl_glfw igl::triangle igl::png TBB::tbb)
