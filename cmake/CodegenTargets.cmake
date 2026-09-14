if(WIN32)
    # Windows antivirus real-time scanning can transiently hold a lock on the
    # multi-megabyte generated file while ApplySkate3CodegenPatches.cmake is
    # rewriting it, causing a spurious "Permission denied". Retry the patch
    # step a few times instead of failing the whole (expensive) codegen run.
    add_custom_target(generate-skate3
        COMMAND $<TARGET_FILE:rex::rexglue> codegen
                ${SKATE3_CODEGEN_ARGS}
                "${CMAKE_CURRENT_BINARY_DIR}/manifests/skate3.toml"
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
                "for ($i = 0; $i -lt 40; $i++) { & '${CMAKE_COMMAND}' '-DSKATE3_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}' -P '${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplySkate3CodegenPatches.cmake'; if ($LASTEXITCODE -eq 0) { exit 0 }; Start-Sleep -Seconds 3 }; exit 1"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Generating recompiled code for default.xex"
        VERBATIM
    )
else()
    add_custom_target(generate-skate3
        COMMAND $<TARGET_FILE:rex::rexglue> codegen
                ${SKATE3_CODEGEN_ARGS}
                "${CMAKE_CURRENT_BINARY_DIR}/manifests/skate3.toml"
        COMMAND "${CMAKE_COMMAND}"
                "-DSKATE3_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplySkate3CodegenPatches.cmake"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Generating recompiled code for default.xex"
        VERBATIM
    )
endif()

add_custom_target(generate-eawebkit
    COMMAND $<TARGET_FILE:rex::rexglue> codegen
            ${SKATE3_CODEGEN_ARGS}
            "${CMAKE_CURRENT_BINARY_DIR}/manifests/eawebkit.toml"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating recompiled code for EAWebkit.xex"
    VERBATIM
)

if(TARGET skate3-title-update-codegen-inputs)
    add_dependencies(generate-skate3 skate3-title-update-codegen-inputs)
    add_dependencies(generate-eawebkit skate3-title-update-codegen-inputs)
endif()

add_dependencies(generate-skate3 generate-eawebkit)

add_custom_target(generate-all
    DEPENDS generate-skate3 generate-eawebkit
)

add_custom_target(skate3_codegen DEPENDS generate-skate3)
add_custom_target(eawebkit_codegen DEPENDS generate-eawebkit)
add_custom_target(all_codegen DEPENDS generate-all)
