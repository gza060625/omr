set(OMR_DDR ON CACHE BOOL "Enable DDR")
set(OMR_EXAMPLE ON CACHE BOOL "")
set(OMR_JIT  ON CACHE BOOL "")
set(OMR_GC ON CACHE BOOL "")
set(OMR_PORT ON CACHE BOOL "")
set(OMR_THREAD ON CACHE BOOL "")
set(OMR_OMRSIG ON CACHE BOOL "")
set(OMR_FVTEST ON CACHE BOOL "")
set(OMR_GLUE ${CMAKE_SOURCE_DIR}/example/glue)
set(OMR_GC_SEGREGATED_HEAP ON CACHE BOOL "")
set(OMR_GC_MODRON_SCAVENGER ON CACHE BOOL "")
set(OMR_GC_MODRON_CONCURRENT_MARK ON CACHE BOOL "")
set(OMR_THR_CUSTOM_SPIN_OPTIONS ON CACHE BOOL "")
set(OMR_NOTIFY_POLICY_CONTROL ON CACHE BOOL "")
set(OMR_THR_SPIN_WAKE_CONTROL ON CACHE BOOL "")
set(OMR_THR_SPIN_CODE_REFACTOR ON CACHE BOOL "")
set(OMR_WARNINGS_AS_ERRORS ON CACHE BOOL "")
