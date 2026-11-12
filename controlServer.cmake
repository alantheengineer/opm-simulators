# Step control server. Generated gRPC code and the server live in their own
# library so that gRPC headers stay out of the simulator sources.

find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

add_library(opmcontrol STATIC
  opm/simulators/flow/control/opm_control.proto
  opm/simulators/flow/control/ControlServer.cpp
)
set_target_properties(opmcontrol PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(opmcontrol PUBLIC cxx_std_20)
target_include_directories(opmcontrol PUBLIC
  $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
)
target_link_libraries(opmcontrol PUBLIC gRPC::grpc++ protobuf::libprotobuf)

protobuf_generate(TARGET opmcontrol
  LANGUAGE cpp
  IMPORT_DIRS ${PROJECT_SOURCE_DIR}
  PROTOC_OUT_DIR ${PROJECT_BINARY_DIR}
)
protobuf_generate(TARGET opmcontrol
  LANGUAGE grpc
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
  PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
  IMPORT_DIRS ${PROJECT_SOURCE_DIR}
  PROTOC_OUT_DIR ${PROJECT_BINARY_DIR}
)

target_link_libraries(opmsimulators PUBLIC opmcontrol)
# Installed and exported with opmsimulators, which links it.
list(APPEND opm-simulators_EXTRA_TARGETS opmcontrol)
target_compile_definitions(opmsimulators PUBLIC HAVE_CONTROL_SERVER=1)

add_executable(control_step_client tests/control/control_step_client.cpp)
target_link_libraries(control_step_client PRIVATE opmcontrol)
