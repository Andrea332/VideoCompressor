# Read by CPack for each kind of package (CPACK_PROJECT_CONFIG_FILE in CMakeLists.txt).
# The zip is the portable version: VideoCompressor.ini next to the program makes it keep its settings there
# instead of in the registry, so it leaves nothing in the system. The installer doesn't have it.
if(CPACK_GENERATOR STREQUAL "ZIP")
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}-portable")
    set(CPACK_INSTALLED_DIRECTORIES "${CPACK_PORTABLE_FILES_DIR};.")
endif()
