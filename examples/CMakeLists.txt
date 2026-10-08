set(VITA_ELF_CREATE_FLAGS "")
set(VITA_MAKE_FSELF_FLAGS "")
enable_language(ASM)
add_executable(perf_test pmu-test/main.c pmu-test/event_coverage.c pmu-test/event_footprint.S)
target_include_directories(perf_test PRIVATE ../include)
target_link_directories(perf_test PRIVATE ${CMAKE_BINARY_DIR}/user/user_stubs)
add_dependencies(perf_test user_stubs kernel_stubs)
target_link_libraries(perf_test ScePerf_stub_weak SceKernelPerf_stub_weak vita2d
  SceDisplay_stub SceGxm_stub SceSysmodule_stub SceCtrl_stub ScePgf_stub
  SceCommonDialog_stub SceAppMgr_stub SceLibKernel_stub SceIofilemgr_stub
  SceKernelThreadMgr_stub m)
vita_create_self(perf-test.self perf_test UNSAFE)
vita_create_vpk(retail-perf-test.vpk PERF00001 perf-test.self
  VERSION 01.00 NAME "Retail CPU PMU test"
  FILE ${CMAKE_BINARY_DIR}/user/libperf.suprx libperf.suprx)

add_executable(perf_stress stress/main.c stress/pipeline.c)
target_include_directories(perf_stress PRIVATE ../include)
target_link_directories(perf_stress PRIVATE ${CMAKE_BINARY_DIR}/user/user_stubs)
add_dependencies(perf_stress user_stubs kernel_stubs)
target_link_libraries(perf_stress ScePerf_stub_weak SceKernelPerf_stub_weak vita2d
  SceDisplay_stub SceGxm_stub SceSysmodule_stub SceCtrl_stub ScePgf_stub
  SceCommonDialog_stub SceAppMgr_stub SceLibKernel_stub SceIofilemgr_stub
  SceKernelThreadMgr_stub m)
vita_create_self(perf-stress.self perf_stress UNSAFE)
vita_create_vpk(libperf-stress.vpk PERFSTR01 perf-stress.self
  VERSION 01.00 NAME "Three-core libperf Stress Lab"
  FILE ${CMAKE_BINARY_DIR}/user/libperf.suprx libperf.suprx)

add_executable(perf_demo profiler/main.c profiler/workloads.c)
target_include_directories(perf_demo PRIVATE ../include)
target_link_directories(perf_demo PRIVATE ${CMAKE_BINARY_DIR}/user/user_stubs)
add_dependencies(perf_demo user_stubs)
set_source_files_properties(profiler/workloads.c PROPERTIES
  COMPILE_OPTIONS "-fno-tree-vectorize;-fno-tree-slp-vectorize")
target_link_libraries(perf_demo ScePerf_stub_weak vita2d
  SceDisplay_stub SceGxm_stub SceSysmodule_stub SceCtrl_stub ScePgf_stub
  SceCommonDialog_stub SceAppMgr_stub SceLibKernel_stub SceIofilemgr_stub
  SceKernelThreadMgr_stub ScePower_stub m)
vita_create_self(perf-demo.self perf_demo UNSAFE)
vita_create_vpk(cpu-function-profiler.vpk PERFDEMO1 perf-demo.self
  VERSION 01.00 NAME "CPU Function Profiler"
  FILE ${CMAKE_BINARY_DIR}/user/libperf.suprx libperf.suprx)
