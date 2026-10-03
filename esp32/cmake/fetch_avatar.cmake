# Copyright (c) 2026 muse-gadget-xiaozhi contributors
# SPDX-License-Identifier: MIT
#
# The Jollybot avatar is Meta's and isn't licensed for redistribution, so this
# repo doesn't carry it. Fetch it from Meta's own repository at a pinned commit
# and check it, once, into avatar/ (gitignored).
set(MUSE_AVATAR_COMMIT 1b56662588c0ea00bdee24b9bcd1835e12848e9a)
set(MUSE_AVATAR_FILES
    "muse_pixel.c=72369c740fb9450a11491a9e5e71a5e031f3a835ab037bf03a2c17914b3fb915"
    "happy_anim.c=547e5201bcf8667a6603d3323d4f973059bbf605f20461bc09f67b30a34c5c21"
    "happy_anim.h=50721fcdbaffdcf277e5b5713dbeb5d98b7f1ec7da8b4da3d67b425e165b34c1")
get_filename_component(_avatar_dir "${CMAKE_CURRENT_LIST_DIR}/../avatar" ABSOLUTE)

foreach(_entry ${MUSE_AVATAR_FILES})
    string(REPLACE "=" ";" _pair "${_entry}")
    list(GET _pair 0 _name)
    list(GET _pair 1 _sha)
    set(_dst "${_avatar_dir}/${_name}")
    if(EXISTS "${_dst}")
        file(SHA256 "${_dst}" _have)
        if(_have STREQUAL _sha)
            continue()
        endif()
    endif()
    message(STATUS "Fetching avatar/${_name} from facebookincubator/muse-gadget-sdk")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/facebookincubator/muse-gadget-sdk/${MUSE_AVATAR_COMMIT}/esp32/avatar/${_name}"
        "${_dst}"
        EXPECTED_HASH SHA256=${_sha}
        STATUS _status)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
        file(REMOVE "${_dst}")
        message(FATAL_ERROR "Couldn't fetch avatar/${_name}: ${_status}. "
            "Download it by hand from github.com/facebookincubator/muse-gadget-sdk/tree/${MUSE_AVATAR_COMMIT}/esp32/avatar")
    endif()
endforeach()
