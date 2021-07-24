# Use idf_component_register to add this component, this will define a CMake library target.
# Specify component sources in SRCS.
# Specify component include directories in INCLUDE_DIRS.
set(srcs "src/foo.c")


idf_component_register(SRCS "${srcs}"
                        INCLUDE_DIRS "include")
# standard CMake function can be used to specify dependencies. ${COMPONENT_TARGET} is defined
# from esp-idf when you call register_component, by default it's idf_component_<folder_name>.
target_link_libraries(
                        ${COMPONENT_TARGET} 
                        PRIVATE 
                        AFR::demo_core_mqtt
                        AFR::common_io
                        )
