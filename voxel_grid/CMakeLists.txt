cmake_minimum_required(VERSION 3.5)
project(voxel_grid)

find_package(ament_cmake REQUIRED)

# Default to C++14
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 14)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(rcl REQUIRED)

include_directories(include ${rcl_INCLUDE_DIRS})

add_library(voxel_grid src/voxel_grid.cpp)
#add_dependencies(voxel_grid ${${PROJECT_NAME}_EXPORTED_TARGETS} ${rclcpp_EXPORTED_TARGETS})
target_link_libraries(voxel_grid ${rcl_LIBRARIES})

ament_export_include_directories(include)
ament_export_libraries(voxel_grid)
ament_package()

install(TARGETS voxel_grid
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
)

install(DIRECTORY include/${PROJECT_NAME}/
  DESTINATION include
)
