add_executable(velox_live velox_live.cpp)
target_link_libraries(velox_live PRIVATE velox_sequencer)

add_executable(velox_gateway velox_gateway.cpp)
target_link_libraries(velox_gateway PRIVATE velox_gateway_lib velox_marketdata)

add_executable(velox_client velox_client.cpp)
target_link_libraries(velox_client PRIVATE velox_protocol velox_asio)

add_executable(velox_viz velox_viz.cpp)
target_link_libraries(velox_viz PRIVATE velox_viz_lib)

add_executable(velox_adminctl velox_adminctl.cpp)
target_link_libraries(velox_adminctl PRIVATE velox_admin)

if(VELOX_BUILD_AUDIT_TIER)
    add_executable(velox_auditd velox_auditd.cpp)
    target_link_libraries(velox_auditd PRIVATE velox_audit)
endif()
