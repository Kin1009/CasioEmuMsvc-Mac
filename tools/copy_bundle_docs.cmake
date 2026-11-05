# Copy only generic app/toolchain documentation into the app bundle and write
# a manifest.  Invoked from CMake POST_BUILD with -DDEST, -DEMU_DOCS,
# -DEMU_ROOT, -DWIZ_DOCS.
#
# This bundle is user-facing: it must never contain environment notes, local
# checkout paths, build scratch paths or internal planning records.  Keep the
# allowlist explicit so new local notes are not picked up automatically.

if(NOT DEFINED DEST OR DEST STREQUAL "")
    return()
endif()

# Relative paths (inside Resources/docs) that are safe to ship.
set(ALLOWED_DOCS
    API.md
    ARCHITECTURE.md
    C-CONFORMANCE.md
    DIRECTIVES.md
    EDITOR.md
    GUIDE.md
    PLUGIN-GUIDE.md
    PLUGIN_SPEC.md
    PRIVACY.md
    ROP.md
    STATUS_ICONS.md
    WIZ-COMPILER.md
    WIZ-UPSTREAM.md
    plugins/EXAMPLES.md
    plugins/STATUS.md
    plugins/TROUBLESHOOTING.md)

# Remove the previous bundle first: excluded documents must not linger after a
# rebuild when their source disappears from the repository.
file(REMOVE_RECURSE "${DEST}")
file(MAKE_DIRECTORY "${DEST}")

set(manifest_lines "path\ttitle\tsize\tmtime\n")

function(doc_is_allowed relative out_var)
    set(allowed FALSE)
    foreach(entry IN LISTS ALLOWED_DOCS)
        if(relative STREQUAL entry)
            set(allowed TRUE)
            break()
        endif()
    endforeach()
    set(${out_var} "${allowed}" PARENT_SCOPE)
endfunction()

function(doc_has_local_environment text out_var)
    set(found FALSE)
    # Absolute home/system paths, local checkout names, ROM folders and local
    # scratch paths are environment-specific by definition.
    foreach(pattern
            "/Users/kin"
            "/Applications/"
            "/private/"
            "/Volumes/"
            "Wiz/preview"
            "CasioEmuMsvc-Mac"
            "models/"
            "Wiz/temp"
            "./temp/"
            "../temp/")
        string(FIND "${text}" "${pattern}" position)
        if(NOT position EQUAL -1)
            set(found TRUE)
            break()
        endif()
    endforeach()
    set(${out_var} "${found}" PARENT_SCOPE)
endfunction()

function(copy_one source relative target_root)
    if(NOT EXISTS "${source}")
        return()
    endif()
    set(check_relative "${relative}")
    if(check_relative MATCHES "^wiz/")
        string(REGEX REPLACE "^wiz/" "" check_relative "${check_relative}")
    endif()
    doc_is_allowed("${check_relative}" allowed)
    if(NOT allowed)
        return()
    endif()
    file(READ "${source}" content)
    doc_has_local_environment("${content}" local_env)
    if(local_env)
        message(STATUS "Skipping environment-specific doc: ${relative}")
        return()
    endif()

    get_filename_component(name "${source}" NAME)
    set(destination "${target_root}/${relative}")
    get_filename_component(destination_dir "${destination}" DIRECTORY)
    file(MAKE_DIRECTORY "${destination_dir}")
    file(COPY_FILE "${source}" "${destination}" ONLY_IF_DIFFERENT)
    file(SIZE "${source}" size)
    file(TIMESTAMP "${source}" mtime "%Y-%m-%dT%H:%M:%SZ" UTC)
    set(title "${name}")
    file(STRINGS "${source}" headings REGEX "^# +")
    if(headings)
        list(GET headings 0 heading)
        string(REGEX REPLACE "^# +" "" heading "${heading}")
        if(NOT heading STREQUAL "")
            set(title "${heading}")
        endif()
    endif()
    string(REPLACE "\t" " " title "${title}")
    string(REPLACE "\n" " " title "${title}")
    set(entry "${relative}\t${title}\t${size}\t${mtime}\n")
    set(manifest_lines "${manifest_lines}${entry}" PARENT_SCOPE)
endfunction()

if(DEFINED EMU_DOCS AND EXISTS "${EMU_DOCS}")
    file(GLOB_RECURSE emu_docs RELATIVE "${EMU_DOCS}" "${EMU_DOCS}/*.md")
    foreach(relative IN LISTS emu_docs)
        copy_one("${EMU_DOCS}/${relative}" "${relative}" "${DEST}")
    endforeach()
endif()

if(DEFINED EMU_ROOT AND EXISTS "${EMU_ROOT}")
    copy_one("${EMU_ROOT}/PRIVACY.md" "PRIVACY.md" "${DEST}")
endif()

if(DEFINED WIZ_DOCS AND EXISTS "${WIZ_DOCS}")
    file(GLOB_RECURSE wiz_docs RELATIVE "${WIZ_DOCS}" "${WIZ_DOCS}/*.md" "${WIZ_DOCS}/*.txt")
    foreach(relative IN LISTS wiz_docs)
        copy_one("${WIZ_DOCS}/${relative}" "wiz/${relative}" "${DEST}")
    endforeach()
endif()

list(REMOVE_DUPLICATES manifest_lines)
list(SORT manifest_lines)
string(REPLACE ";" "" manifest_text "${manifest_lines}")
file(WRITE "${DEST}/manifest.tsv" "${manifest_text}")
