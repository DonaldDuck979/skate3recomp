if(NOT DEFINED SKATE3_SOURCE_DIR)
  message(FATAL_ERROR "SKATE3_SOURCE_DIR is required")
endif()

file(GLOB _skate3_recomp_files
  LIST_DIRECTORIES false
  "${SKATE3_SOURCE_DIR}/generated/skate3_recomp.*.cpp")
if(NOT _skate3_recomp_files)
  message(FATAL_ERROR "No generated Skate 3 recompilation files found")
endif()

function(_skate3_add_include _contents_var _include)
  set(_contents "${${_contents_var}}")
  if(NOT _contents MATCHES "#include \"${_include}\"")
    string(REPLACE
      "#include \"skate3_init.h\"\n"
      "#include \"skate3_init.h\"\n#include \"${_include}\"\n"
      _contents
      "${_contents}")
  endif()
  set(${_contents_var} "${_contents}" PARENT_SCOPE)
endfunction()

set(_frustum_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "Skate3UltrawideGameFrustumPatchScope")
    set(_frustum_patched TRUE)
    break()
  endif()
  string(FIND "${_contents}" "ctx.r6.u64 = REX_LOAD_U32(ctx.r4.u32 + 5260);" _frustum_anchor)
  if(_frustum_anchor EQUAL -1)
    continue()
  endif()

  string(SUBSTRING "${_contents}" ${_frustum_anchor} 12000 _frustum_window)
  string(REGEX MATCH
    "\t// bl 0x[0-9a-fA-F]+\n\tctx\\.lr = 0x[0-9A-F]+;\n\tsub_[0-9A-F]+\\(ctx, base\\);"
    _frustum_call
    "${_frustum_window}")
  if(_frustum_call STREQUAL "")
    message(FATAL_ERROR "Failed to apply Skate 3 generated frustum patch; call near frustum anchor not found in ${_file}")
  endif()

  string(REGEX REPLACE
    "(\tctx\\.lr = 0x[0-9A-F]+;\n)"
    "\\1\tSkate3UltrawideGameFrustumPatchScope skate3_ultrawide_game_frustum_patch_scope(\n\t\tctx, base, ctx.r4.u32);\n"
    _frustum_patch
    "${_frustum_call}")
  string(REPLACE "${_frustum_call}" "${_frustum_patch}" _contents "${_contents}")
  _skate3_add_include(_contents "skate3_ultrawide_guest.h")
  file(WRITE "${_file}" "${_contents}")
  set(_frustum_patched TRUE)
  message(STATUS "Applied Skate 3 generated frustum patch in ${_file}")
  break()
endforeach()
if(NOT _frustum_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 generated frustum patch; frustum anchor not found")
endif()

set(_fov_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "Skate3MaybeOverrideProjectionFovRadians")
    set(_fov_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "ctx\\.f27\\.f64 = ctx\\.f1\\.f64;")
    continue()
  endif()
  if(NOT _contents MATCHES "ctx\\.f4\\.f64 = double\\(float\\(ctx\\.f1\\.f64 \\* ctx\\.f0\\.f64\\)\\);")
    continue()
  endif()

  set(_projection_fov_site "ctx.f27.f64 = ctx.f1.f64;")
  set(_projection_fov_patch
"ctx.f1.f64 = double(Skate3MaybeOverrideProjectionFovRadians(float(ctx.f1.f64)));
	ctx.f27.f64 = ctx.f1.f64;")
  string(REPLACE "${_projection_fov_site}" "${_projection_fov_patch}" _contents "${_contents}")
  _skate3_add_include(_contents "skate3_fov.h")
  file(WRITE "${_file}" "${_contents}")
  set(_fov_patched TRUE)
  message(STATUS "Applied Skate 3 generated projection FOV patch in ${_file}")
  break()
endforeach()
if(NOT _fov_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 generated projection FOV patch; projection FOV anchor not found")
endif()

set(_demo_path_movie_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "ShouldForceIntroMovieComplete")
    set(_demo_path_movie_patched TRUE)
    break()
  endif()

  set(_demo_path_movie_site
"	// bl 0x825d60c8
	ctx.lr = 0x825E05A0;
	sub_825D60C8(ctx, base);")
  if(NOT _contents MATCHES "ctx\\.lr = 0x825E05A0;")
    continue()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_825E0510\\)")
    continue()
  endif()
  string(FIND "${_contents}" "${_demo_path_movie_site}" _demo_path_movie_anchor)
  if(_demo_path_movie_anchor EQUAL -1)
    continue()
  endif()

  set(_demo_path_movie_patch
"	if (skate3::demo_path::ShouldForceIntroMovieComplete()) {
		ctx.r3.u64 = 0;
	} else {
		// bl 0x825d60c8
		ctx.lr = 0x825E05A0;
		sub_825D60C8(ctx, base);
	}")
  string(REPLACE "${_demo_path_movie_site}" "${_demo_path_movie_patch}" _contents "${_contents}")
  _skate3_add_include(_contents "skate3_demo_path.h")
  file(WRITE "${_file}" "${_contents}")
  set(_demo_path_movie_patched TRUE)
  message(STATUS "Applied Skate 3 demo path intro movie patch in ${_file}")
  break()
endforeach()
if(NOT _demo_path_movie_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 demo path intro movie patch; FEMoviePlayer::Update anchor not found")
endif()

# ---- Open Roam (full-map online freeskate) -------------------------------
# Each patch inserts a call into src/skate3_oob_watch.cpp (declared in
# skate3_open_roam_guest.h). The calls are no-ops unless skate3_oob_kill_mode
# enables the feature. A patch is skipped if its marker is already present.
function(_skate3_open_roam_patch _name _marker _anchor _replacement)
  foreach(_file IN LISTS _skate3_recomp_files)
    file(READ "${_file}" _contents)
    string(FIND "${_contents}" "${_marker}" _marker_pos)
    if(NOT _marker_pos EQUAL -1)
      return()
    endif()
    string(FIND "${_contents}" "${_anchor}" _anchor_pos)
    if(_anchor_pos EQUAL -1)
      continue()
    endif()
    string(REPLACE "${_anchor}" "${_replacement}" _contents "${_contents}")
    _skate3_add_include(_contents "skate3_open_roam_guest.h")
    file(WRITE "${_file}" "${_contents}")
    message(STATUS "Applied Open Roam patch '${_name}' in ${_file}")
    return()
  endforeach()
  message(FATAL_ERROR "Failed to apply Open Roam patch '${_name}'; anchor not found")
endfunction()

# Collision streams around the player: streaming-channel focus setter
# sub_8247C100, right before it tests the channel lock count (r11).
_skate3_open_roam_patch("focus"
  "Skate3OpenRoam_FocusUpdate(ctx, base);"
"	// mr r11,r11
	ctx.r11.u64 = ctx.r11.u64;
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x8247c160"
"	// mr r11,r11
	ctx.r11.u64 = ctx.r11.u64;
	Skate3OpenRoam_FocusUpdate(ctx, base);
	// cmplwi cr6,r11,0
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	// beq cr6,0x8247c160")

# Sim_LocalPlayerStreamerController::Update (sub_8272AB80): skip its
# online/game-state gates.
_skate3_open_roam_patch("sim-update-gate"
  "if (Skate3OpenRoam_SimFollow()) goto loc_8272ABB8;"
"	ctx.lr = 0x8272AB90;
	sub_82743FC0(ctx, base);
"
"	ctx.lr = 0x8272AB90;
	sub_82743FC0(ctx, base);
	if (Skate3OpenRoam_SimFollow()) goto loc_8272ABB8;
")

# sub_8285CDC0: create Sim_LocalPlayerStreamerController in online sessions
# too (byte +323 of the session globals = online).
_skate3_open_roam_patch("sim-controller"
  "if (!ctx.cr6.eq && !Skate3OpenRoam_SimFollow()) goto loc_8285CE88;"
"	// bne cr6,0x8285ce88
	if (!ctx.cr6.eq) goto loc_8285CE88;
	// lis r11,-31997"
"	// bne cr6,0x8285ce88
	if (!ctx.cr6.eq && !Skate3OpenRoam_SimFollow()) goto loc_8285CE88;
	// lis r11,-31997")

# sub_825DE4E0 (out-of-boundary message): skip showing ID_ONLINE_BOUNDARY_WARN.
_skate3_open_roam_patch("boundary-warning"
  "if (Skate3OpenRoam_HideAreaWarning()) goto loc_825DE718;"
"loc_825DE670:
"
"loc_825DE670:
	if (Skate3OpenRoam_HideAreaWarning()) goto loc_825DE718;
")

# sub_82D55710 (DisableSessionMarker): markers stay usable out of the area.
_skate3_open_roam_patch("marker-disable"
  "if (Skate3OpenRoam_IgnoreMarkerDisable(ctx, base)) return;"
"DEFINE_REX_FUNC(sub_82D55710) {
	REX_FUNC_PROLOGUE();
	uint32_t ea{};
"
"DEFINE_REX_FUNC(sub_82D55710) {
	REX_FUNC_PROLOGUE();
	uint32_t ea{};
	if (Skate3OpenRoam_IgnoreMarkerDisable(ctx, base)) return;
")
# ---- Cosmetics: character part sets (src/skate3_cosmetic_lock.cpp) ----------
_skate3_open_roam_patch("set-part"
  "if (Skate3Cosmetic_OnSetPart(ctx, base, 0)) return;"
"DEFINE_REX_FUNC(sub_82DDEDB0) {
	REX_FUNC_PROLOGUE();
	uint32_t ea{};
"
"DEFINE_REX_FUNC(sub_82DDEDB0) {
	REX_FUNC_PROLOGUE();
	uint32_t ea{};
	if (Skate3Cosmetic_OnSetPart(ctx, base, 0)) return;
")

_skate3_open_roam_patch("swap-part"
  "Skate3Cosmetic_OnSetPart(ctx, base, 1);"
"DEFINE_REX_FUNC(sub_82DDEEF8) {
	REX_FUNC_PROLOGUE();
	uint32_t ea{};
"
"DEFINE_REX_FUNC(sub_82DDEEF8) {
	REX_FUNC_PROLOGUE();
	uint32_t ea{};
	Skate3Cosmetic_OnSetPart(ctx, base, 1);
")
