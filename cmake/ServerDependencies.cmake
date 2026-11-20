# The servers' dependencies: the PostgreSQL client library, libpq. Not
# fetched (it comes with PostgreSQL): on Windows the EDB installer's (C:\Program Files\PostgreSQL\<n>),
# on Linux the distribution's package.
if(WIN32 AND NOT PostgreSQL_ROOT)
    file(GLOB _sr_pg_roots "C:/Program Files/PostgreSQL/*")
    list(SORT _sr_pg_roots COMPARE NATURAL ORDER DESCENDING)
    foreach(root IN LISTS _sr_pg_roots)
        if(EXISTS "${root}/include/libpq-fe.h")
            set(PostgreSQL_ROOT "${root}")
            break()
        endif()
    endforeach()
endif()
if(WIN32 AND PostgreSQL_ROOT AND EXISTS "${PostgreSQL_ROOT}/lib/libpq.lib")
    # The DLL's import library; the installer's libpq.a is a partial static archive that clang++
    # (GNU front end) would otherwise prefer.
    set(PostgreSQL_LIBRARY "${PostgreSQL_ROOT}/lib/libpq.lib" CACHE FILEPATH "" FORCE)
endif()
find_package(PostgreSQL 14 REQUIRED)

# Copies libpq and the DLLs it needs next to an executable (Windows). libpq also needs the OpenSSL
# DLLs; sr_copy_openssl_runtime puts ours there (OpenSSL 3 keeps one ABI across 3.x).
function(sr_copy_postgresql_runtime target)
    if(NOT WIN32)
        return()
    endif()
    get_filename_component(pg_root "${PostgreSQL_INCLUDE_DIR}" DIRECTORY)
    # libpq is needed; the others only by the releases that link them (18's libpq no longer needs libwinpthread).
    foreach(dll libpq.dll libintl-9.dll libiconv-2.dll libwinpthread-1.dll)
        find_file(SR_PG_DLL_${dll} NAMES ${dll} PATHS "${pg_root}/bin" NO_DEFAULT_PATH)
        if(NOT SR_PG_DLL_${dll})
            if(dll STREQUAL "libpq.dll")
                message(FATAL_ERROR "PostgreSQL runtime ${dll} not found under ${pg_root}/bin")
            endif()
            continue()
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SR_PG_DLL_${dll}}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endforeach()
endfunction()
