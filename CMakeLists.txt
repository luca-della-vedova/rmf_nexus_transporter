cmake_minimum_required(VERSION 3.8)
project(rmf_nexus_transporter)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# find dependencies
find_package(ament_cmake REQUIRED)
find_package(ament_cmake_ros REQUIRED)
find_package(nexus_endpoints REQUIRED)
find_package(nexus_transporter REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(pluginlib REQUIRED)
find_package(rmf_dispenser_msgs REQUIRED)
find_package(rmf_ingestor_msgs REQUIRED)
find_package(rmf_task_msgs REQUIRED)
find_package(rmf_utils REQUIRED)
find_package(std_msgs REQUIRED)

add_library(rmf_nexus_transporter src/rmf_nexus_transporter.cpp)
add_library(rmf_nexus_transporter::rmf_nexus_transporter ALIAS rmf_nexus_transporter)
target_compile_features(rmf_nexus_transporter PUBLIC c_std_99 cxx_std_17)  # Require C99 and C++17
target_include_directories(rmf_nexus_transporter PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include/${PROJECT_NAME}>)
ament_target_dependencies(
  rmf_nexus_transporter
  PUBLIC
    nexus_endpoints
    nexus_transporter
    nlohmann_json
    pluginlib
    rmf_dispenser_msgs
    rmf_ingestor_msgs
    rmf_task_msgs
    rmf_utils
    std_msgs
)

# Causes the visibility macros to use dllexport rather than dllimport,
# which is appropriate when building the dll but not consuming it.
target_compile_definitions(rmf_nexus_transporter PRIVATE "RMF_NEXUS_TRANSPORTER_BUILDING_LIBRARY")

install(
  DIRECTORY include/
  DESTINATION include/${PROJECT_NAME}
)
install(
  TARGETS rmf_nexus_transporter
  EXPORT ${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  # the following line skips the linter which checks for copyrights
  # comment the line when a copyright and license is added to all source files
  set(ament_cmake_copyright_FOUND TRUE)
  # the following line skips cpplint (only works in a git repo)
  # comment the line when this package is in a git repo and when
  # a copyright and license is added to all source files
  set(ament_cmake_cpplint_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()
endif()

pluginlib_export_plugin_description_file(rmf_nexus_transporter transporter_plugin.xml)

ament_export_include_directories(
  "include/${PROJECT_NAME}"
)
ament_export_targets(
  ${PROJECT_NAME}
  HAS_LIBRARY_TARGET
)
ament_export_dependencies(
  nexus_transporter
)

ament_package()
