# The layering of the modules, checked at configure time: a target that links
# (directly or through other targets) something its layer must not see stops the configure.

# All targets `target` links, transitively (aliases resolved to real names).
function(sr_link_closure target out)
    set(queue ${target})
    set(seen "")
    while(queue)
        list(POP_FRONT queue name)
        if(NOT TARGET ${name})
            continue()
        endif()
        get_target_property(real ${name} ALIASED_TARGET)
        if(real)
            set(name ${real})
        endif()
        if(name IN_LIST seen)
            continue()
        endif()
        list(APPEND seen ${name})
        foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(libs ${name} ${property})
            if(NOT libs)
                continue()
            endif()
            foreach(lib IN LISTS libs)
                # Static libraries list their private links as $<LINK_ONLY:target>.
                string(REGEX REPLACE "^\\$<LINK_ONLY:(.+)>$" "\\1" lib "${lib}")
                if(NOT lib MATCHES "^\\$<")
                    list(APPEND queue ${lib})
                endif()
            endforeach()
        endforeach()
    endwhile()
    set(${out} ${seen} PARENT_SCOPE)
endfunction()

# Fails the configure if `target` reaches any of the targets after it. Targets that do not
# exist in this configuration are ignored.
function(sr_forbid_links target)
    if(NOT TARGET ${target})
        return()
    endif()
    sr_link_closure(${target} closure)
    foreach(forbidden IN LISTS ARGN)
        if(NOT TARGET ${forbidden})
            continue()
        endif()
        get_target_property(real ${forbidden} ALIASED_TARGET)
        if(NOT real)
            set(real ${forbidden})
        endif()
        if(real IN_LIST closure)
            message(FATAL_ERROR
                "Layering: ${target} must not depend on ${forbidden}. "
                "It reaches: ${closure}")
        endif()
    endforeach()
endfunction()

function(sr_check_dependency_rules)
    # The simulation never sees rendering, audio, UI, the window or their libraries.
    sr_forbid_links(sr::game
        sr::rhi sr::render sr::gfx sr::anim sr::audio sr::audio_data sr::ui sr::devui sr::platform
        SDL3::SDL3-static imgui OpenAL::OpenAL Vulkan::Vulkan)
    # The servers never see rendering, audio, UI or the window either.
    foreach(server_target sr::server sr::gateway sr::login sr::gameserver sr::web SroGateway SroLogin SroGame SroWeb)
        sr_forbid_links(${server_target}
            sr::rhi sr::render sr::gfx sr::anim sr::audio sr::audio_data sr::ui sr::devui sr::platform
            SDL3::SDL3-static imgui OpenAL::OpenAL Vulkan::Vulkan)
    endforeach()
    # Our connections never reach the HTTP library (the website's API alone uses it).
    sr_forbid_links(sr::net httplib::httplib)
    # Below the client, no module reaches the window or the device except where it must.
    sr_forbid_links(sr::core sr::assets sr::net sr::ui sr::platform sr::rhi sr::audio)
    sr_forbid_links(sr::assets sr::net sr::ui sr::platform sr::rhi sr::render sr::audio)
    sr_forbid_links(sr::net sr::assets sr::ui sr::platform sr::rhi sr::render sr::audio)
    sr_forbid_links(sr::audio sr::rhi sr::render sr::ui sr::platform sr::net SDL3::SDL3-static Vulkan::Vulkan)
    sr_forbid_links(sr::rhi sr::render sr::audio sr::ui sr::assets sr::net SDL3::SDL3-static)
    # The renderer and animation never reach the old renderer (engine/render), the window or the UI.
    sr_forbid_links(sr::gfx sr::render sr::ui sr::platform sr::audio sr::devui SDL3::SDL3-static)
    sr_forbid_links(sr::anim sr::render sr::rhi sr::gfx sr::ui sr::platform Vulkan::Vulkan)
endfunction()
