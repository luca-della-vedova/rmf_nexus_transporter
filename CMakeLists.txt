cmake_minimum_required(VERSION 3.8)
project(rmf_nexus_transporter)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# find dependencies
find_package(ament_cmake REQUIRED)
find_package(ament_cmake_ros REQUIRED)
find_package(nexus_endpoints REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(rclcpp_components REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)
find_package(rmf_dispenser_msgs REQUIRED)
find_package(rmf_ingestor_msgs REQUIRED)
find_package(rmf_task_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(yaml_cpp_vendor REQUIRED)
find_package(yaml-cpp REQUIRED)

add_library(${PROJECT_NAME}_plugin src/rmf_nexus_transporter.cpp)
target_compile_features(${PROJECT_NAME}_plugin PUBLIC c_std_99 cxx_std_17)  # Require C99 and C++17
target_compile_definitions(${PROJECT_NAME}_plugin PUBLIC "COMPOSITION_BUILDING_DLL")
target_include_directories(${PROJECT_NAME}_plugin PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include/${PROJECT_NAME}>)

target_link_libraries(
  ${PROJECT_NAME}_plugin
  PUBLIC
    nexus_endpoints::nexus_endpoints
    nlohmann_json::nlohmann_json
    rclcpp_lifecycle::rclcpp_lifecycle
    rclcpp_components::component
    ${rmf_dispenser_msgs_TARGETS}
    ${rmf_ingestor_msgs_TARGETS}
    ${rmf_task_msgs_TARGETS}
    ${std_msgs_TARGETS}
    yaml-cpp::yaml-cpp
)

rclcpp_components_register_node(${PROJECT_NAME}_plugin
  PLUGIN "rmf_nexus_transporter::RmfNexusTransporter"
  EXECUTABLE ${PROJECT_NAME}
)

install(
  DIRECTORY include/
  DESTINATION include/${PROJECT_NAME}
)
install(
  TARGETS ${PROJECT_NAME}_plugin
  EXPORT ${PROJECT_NAME}_targets
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

ament_export_include_directories(
  "include/${PROJECT_NAME}"
)
ament_export_targets(
  ${PROJECT_NAME}_targets
  HAS_LIBRARY_TARGET
)

ament_package()
