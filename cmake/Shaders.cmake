# HLSL -> SPIR-V with DXC (from the Vulkan SDK), embedded into the binary as a
# byte array so executables need no shader files next to them.
#
#   sr_add_shader(<target> SOURCE <file.hlsl> ENTRY <fn> PROFILE <vs_6_0|ps_6_0|...> NAME <cIdentifier>)
#
# Produces <NAME>.spv.h in the target's include path, declaring
#   alignas(4) inline constexpr unsigned char <NAME>[] = {...};

set(SR_EMBED_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/EmbedFile.cmake)

function(sr_add_shader TARGET)
    cmake_parse_arguments(ARG "" "SOURCE;ENTRY;PROFILE;NAME" "" ${ARGN})
    get_filename_component(src ${ARG_SOURCE} ABSOLUTE)
    # Shared includes next to the shader (Frame.hlsli ...) rebuild it too.
    get_filename_component(srcDir ${src} DIRECTORY)
    file(GLOB includes ${srcDir}/*.hlsli)
    set(outDir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    set(spv ${outDir}/${ARG_NAME}.spv)
    set(header ${outDir}/${ARG_NAME}.spv.h)

    add_custom_command(
        OUTPUT ${header}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${outDir}
        COMMAND ${Vulkan_dxc_EXECUTABLE} -spirv -fspv-target-env=vulkan1.3 -WX
                -T ${ARG_PROFILE} -E ${ARG_ENTRY} -Fo ${spv} ${src}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${spv} -DOUTPUT=${header} -DNAME=${ARG_NAME} -P ${SR_EMBED_SCRIPT}
        DEPENDS ${src} ${includes} ${SR_EMBED_SCRIPT}
        COMMENT "Compiling shader ${ARG_NAME} (${ARG_PROFILE})"
        VERBATIM)

    target_sources(${TARGET} PRIVATE ${header})
    target_include_directories(${TARGET} PRIVATE ${outDir})
endfunction()
