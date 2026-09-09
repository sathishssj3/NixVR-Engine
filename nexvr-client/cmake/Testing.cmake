enable_testing()

function(add_nexvr_gtest target source)
    add_executable(${target} ${source})
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS} ${OPENXR_INCLUDE_DIR})
    target_link_libraries(${target} PRIVATE NexVRCore gtest_main gmock Vulkan::Vulkan)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

add_nexvr_gtest(test_compatibility_scorer ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compatibility_scorer.cpp)
add_nexvr_gtest(test_engine_detector ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_engine_detector.cpp)
add_nexvr_gtest(test_frame_timing_manager ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_frame_timing_manager.cpp)
add_nexvr_gtest(test_logger ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_logger.cpp)
add_nexvr_gtest(test_game_profiles_regression ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_game_profiles_regression.cpp)

add_executable(test_adaptive_quality ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_adaptive_quality.cpp)
# PROJECT_SOURCE_DIR (not just /src) because this suite includes headers by
# repo-relative path, e.g. #include "src/rendering/adaptive_quality_controller.h".
target_include_directories(test_adaptive_quality PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src ${ONNXRUNTIME_INCLUDE_DIR})
target_link_directories(test_adaptive_quality PRIVATE ${ONNXRUNTIME_LIB_DIR})
target_link_libraries(test_adaptive_quality PRIVATE NexVRCore onnxruntime gtest_main)
add_test(NAME AdaptiveQualityTest COMMAND test_adaptive_quality)

add_nexvr_gtest(test_vulkan_gpu_profiler ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_vulkan_gpu_profiler.cpp)
add_executable(test_ai_directml ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ai_directml.cpp)
target_include_directories(test_ai_directml PRIVATE ${PROJECT_SOURCE_DIR}/src ${ONNXRUNTIME_INCLUDE_DIR})
target_link_directories(test_ai_directml PRIVATE ${ONNXRUNTIME_LIB_DIR})
target_link_libraries(test_ai_directml PRIVATE NexVRCore onnxruntime)
add_test(NAME AiDirectMLTest COMMAND test_ai_directml)

add_executable(test_ai_ui_synthesizer ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ai_ui_synthesizer.cpp)
target_include_directories(test_ai_ui_synthesizer PRIVATE ${PROJECT_SOURCE_DIR}/src ${ONNXRUNTIME_INCLUDE_DIR})
target_link_directories(test_ai_ui_synthesizer PRIVATE ${ONNXRUNTIME_LIB_DIR})
target_link_libraries(test_ai_ui_synthesizer PRIVATE NexVRCore onnxruntime)
add_test(NAME AiUISynthesizerTest COMMAND test_ai_ui_synthesizer)

add_executable(test_tensor_bridge ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tensor_bridge.cpp)
target_include_directories(test_tensor_bridge PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR} ${ONNXRUNTIME_INCLUDE_DIR})
target_link_directories(test_tensor_bridge PRIVATE ${ONNXRUNTIME_LIB_DIR})
target_link_libraries(test_tensor_bridge PRIVATE NexVRCore onnxruntime gtest_main)
add_test(NAME TensorBridgeTest COMMAND test_tensor_bridge)

add_nexvr_gtest(test_vulkan_async_scheduler ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_vulkan_async_scheduler.cpp)
add_nexvr_gtest(test_cross_backend_ai ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cross_backend_ai.cpp)

file(GLOB_RECURSE VR_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/vr/*.cpp")
file(GLOB_RECURSE VR_RENDERING_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/rendering/*.cpp")

add_nexvr_gtest(test_vulkan_barrier_optimizer ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_vulkan_barrier_optimizer.cpp)
add_nexvr_gtest(test_vulkan_memory_budget ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_vulkan_memory_budget.cpp)
add_nexvr_gtest(test_vulkan_multiple_queues ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_vulkan_multiple_queues.cpp)
add_nexvr_gtest(test_frame_graph_profiler ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_frame_graph_profiler.cpp)
add_nexvr_gtest(test_vulkan_performance_snapshot ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_vulkan_performance_snapshot.cpp)
add_nexvr_gtest(test_vulkan_timeline_semaphore ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_vulkan_timeline_semaphore.cpp)
add_executable(dummy_json ${CMAKE_CURRENT_SOURCE_DIR}/src/test_apps/dummy_json.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/core/sprint_compatibility_logger.cpp)

target_include_directories(dummy_json PRIVATE ${JSON_INCLUDE_DIR} ${PROJECT_SOURCE_DIR}/src ${Vulkan_INCLUDE_DIRS} ${OPENXR_INCLUDE_DIR})
target_link_libraries(dummy_json PRIVATE NexVRCore Vulkan::Vulkan)

add_executable(stress_injector ${CMAKE_CURRENT_SOURCE_DIR}/tests/stress_injector.cpp)
target_include_directories(stress_injector PRIVATE ${PROJECT_SOURCE_DIR}/src ${Vulkan_INCLUDE_DIRS} ${OPENXR_INCLUDE_DIR})
if(MSVC)
    target_compile_options(stress_injector PRIVATE /EHa)
    target_compile_options(NexVRCore PRIVATE /EHa)
endif()
target_link_libraries(stress_injector PRIVATE NexVRCore)

# ---------------------------------------------------------------------------
# Auto-register standard GoogleTest suites
# ---------------------------------------------------------------------------
# Historically ~65 tests/test_*.cpp files existed on disk but were never wired
# into the build, so they silently never ran. This block registers every
# standard gtest suite automatically: any tests/test_*.cpp not already
# registered explicitly above (TARGET guard) and not a standalone main()
# harness (exclude list). CONFIGURE_DEPENDS re-runs the glob on build, so a new
# test file is picked up without a manual reconfigure. Suites needing ONNX
# Runtime or a custom main() stay registered explicitly above and are skipped
# here by the TARGET guard.
file(GLOB AUTO_GTEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp")

# Standalone harnesses that provide their own main() — linking gtest_main would
# duplicate the main symbol. Excluded from gtest auto-registration.
set(AUTO_GTEST_EXCLUDES
    test_openxr_dx12_swapchain
    test_stereo_visual
)

foreach(test_src IN LISTS AUTO_GTEST_SOURCES)
    get_filename_component(test_name ${test_src} NAME_WE)
    if(TARGET ${test_name})
        continue()
    endif()
    if(test_name IN_LIST AUTO_GTEST_EXCLUDES)
        continue()
    endif()
    add_nexvr_gtest(${test_name} ${test_src})
endforeach()
