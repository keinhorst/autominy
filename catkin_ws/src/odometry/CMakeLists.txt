cmake_minimum_required(VERSION 2.8.3)
project(odometry)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


find_package(catkin REQUIRED COMPONENTS nav_msgs std_msgs roscpp tf autominy_msgs)

catkin_package(
    CATKIN_DEPENDS
        nav_msgs
        std_msgs
        autominy_msgs
)


include_directories(${catkin_INCLUDE_DIRS})

# Build the executable
add_executable(odometry_publisher src/odometry_publisher.cpp)
add_dependencies(odometry_publisher ${catkin_EXPORTED_TARGETS})

# Link against the catkin libraries
target_link_libraries(odometry_publisher ${catkin_LIBRARIES})

# Install the executable
install(TARGETS odometry_publisher
  ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION})
## Mark cpp header files for installation
install(DIRECTORY cfg launch
  DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION}
  USE_SOURCE_PERMISSIONS
)
