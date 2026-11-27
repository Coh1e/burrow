# libarchive 3.7.7 checks ZIP sizes and CRCs across headers but reads the local
# filename without comparing it with the central directory. Extend its existing
# seekable reader so ZIP64 offsets, data descriptors and comments still follow
# the single upstream parser. Only workspace archives enable the extra check.
function(brw_check_zip_names)
  set(unit archive_read_support_format_zip.c)
  set(original "${archive_source}/libarchive/${unit}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
  file(READ "${original}" source)
  string(SHA256 digest "${source}")
  if(NOT digest STREQUAL "56c34fa49340589520379a33bd850f67eddb352cbb7d90206c70ce7a4bd40e39")
    message(FATAL_ERROR "Review the ZIP name checks before updating the pinned libarchive reader")
  endif()

  string(REPLACE "/* Our extra markers. */" "/* Our extra markers. */\n\tuint16_t central_name_length;" source "${source}")
  string(REPLACE "struct zip {" "struct zip {\n\tint check_member_names;\n\tuint64_t expected_entries;" source "${source}")
  string(REPLACE [=[} else if (strcmp(key, "mac-ext") == 0) {]=]
    [=[} else if (strcmp(key, "check-member-names") == 0) {
		zip->check_member_names = val != NULL && val[0] != 0;
		return (ARCHIVE_OK);
	} else if (strcmp(key, "mac-ext") == 0) {]=] source "${source}")

  # Keep the original bytes with the central entry until the local header is
  # visited. Each name already fits in its unsigned 16-bit ZIP length field.
  string(REPLACE [=[zip_entry = calloc(1, sizeof(struct zip_entry));]=]
    [=[zip_entry = calloc(1, sizeof(struct zip_entry) +
		    (zip->check_member_names ? archive_le16dec(p + 28) : 0));]=] source "${source}")
  string(REPLACE [=[if (ARCHIVE_OK != process_extra(a, entry, p + filename_length,]=]
    [=[if (zip->check_member_names) {
			zip_entry->central_name_length = (uint16_t)filename_length;
			memcpy(zip_entry + 1, p, filename_length);
		}
		if (ARCHIVE_OK != process_extra(a, entry, p + filename_length,]=] source "${source}")
  string(REPLACE [=[if (zip_entry->zip_flags & ZIP_UTF8_NAME) {]=]
    [=[if (zip->check_member_names && (zip_entry->flags & LA_FROM_CENTRAL_DIRECTORY) &&
	    (filename_length != zip_entry->central_name_length ||
	     memcmp(h, zip_entry + 1, filename_length) != 0)) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
		    "ZIP member name differs from central directory");
		return ARCHIVE_FATAL;
	}
	if (zip_entry->zip_flags & ZIP_UTF8_NAME) {]=] source "${source}")
  # Every directory record in a verified workspace ZIP describes one local
  # entry. Include ordinary macOS sidecar files on all hosts.
  string(REPLACE [=[if (!zip->process_mac_extensions) {]=]
    [=[if (zip->check_member_names) {
			if (!__archive_rb_tree_insert_node(&zip->tree, &zip_entry->node)) {
				archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
				    "ZIP directory records share a local header");
				return ARCHIVE_FATAL;
			}
		} else if (!zip->process_mac_extensions) {]=] source "${source}")
  string(REPLACE "zip->central_directory_offset = cd_offset;"
    "zip->expected_entries = archive_le16dec(p + 10);\n\tzip->central_directory_offset = cd_offset;" source "${source}")
  string(REPLACE "zip->central_directory_offset = archive_le64dec(p + 48);"
    "zip->expected_entries = archive_le64dec(p + 32);\n\tzip->central_directory_offset = archive_le64dec(p + 48);" source "${source}")
  # The longest legal ZIP comment precedes EOF by 65535 bytes. Include room
  # for the 22-byte end record and its optional 20-byte ZIP64 locator.
  string(REPLACE "zipmin(1024 * 16, file_size)"
    "zipmin(zip->check_member_names ? 65535 + 22 + 20 : 1024 * 16, file_size)" source "${source}")
  string(REPLACE [=[if (memcmp(p, "PK\006\006", 4) == 0
		    || memcmp(p, "PK\005\006", 4) == 0) {
			break;]=] [=[if (memcmp(p, "PK\006\006", 4) == 0
		    || memcmp(p, "PK\005\006", 4) == 0) {
			if (zip->check_member_names &&
			    zip->central_directory_entries_total != zip->expected_entries) {
				archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
				    "ZIP directory entry count mismatch");
				return ARCHIVE_FATAL;
			}
			break;]=] source "${source}")

  set(patched "${CMAKE_CURRENT_BINARY_DIR}/${unit}")
  file(CONFIGURE OUTPUT "${patched}" CONTENT "${source}" @ONLY)
  get_target_property(units archive_static SOURCES)
  list(REMOVE_ITEM units "${unit}")
  list(APPEND units "${patched}")
  set_property(TARGET archive_static PROPERTY SOURCES "${units}")
endfunction()

brw_check_zip_names()
