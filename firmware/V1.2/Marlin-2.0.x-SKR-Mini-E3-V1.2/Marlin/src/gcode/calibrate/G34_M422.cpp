/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2019 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "../../inc/MarlinConfig.h"

#if ENABLED(Z_STEPPER_AUTO_ALIGN)

#include "../gcode.h"
#include "../../module/delta.h"
#include "../../module/motion.h"
#include "../../module/stepper.h"
#include "../../module/endstops.h"

#if HOTENDS > 1
  #include "../../module/tool_change.h"
#endif

#if HAS_BED_PROBE
  #include "../../module/probe.h"
#endif

#if HAS_LEVELING
  #include "../../feature/bedlevel/bedlevel.h"
#endif

#define DEBUG_OUT ENABLED(DEBUG_LEVELING_FEATURE)
#include "../../core/debug_out.h"

// Sanity-check
constexpr xy_pos_t sanity_arr_z_align[] = Z_STEPPER_ALIGN_XY;
static_assert(COUNT(sanity_arr_z_align) == Z_STEPPER_COUNT,
  #if ENABLED(Z_TRIPLE_STEPPER_DRIVERS)
    "Z_STEPPER_ALIGN_XY requires three {X,Y} entries (Z, Z2, and Z3)."
  #else
    "Z_STEPPER_ALIGN_XY requires two {X,Y} entries (Z and Z2)."
  #endif
);

xy_pos_t z_auto_align_pos[Z_STEPPER_COUNT] = Z_STEPPER_ALIGN_XY;

inline void set_all_z_lock(const bool lock) {
  stepper.set_z_lock(lock);
  stepper.set_z2_lock(lock);
  #if ENABLED(Z_TRIPLE_STEPPER_DRIVERS)
    stepper.set_z3_lock(lock);
  #endif
}

/**
 * G34: Z-Stepper automatic alignment
 *
 * Parameters: I<iterations> T<accuracy> A<amplification>
 */
void GcodeSuite::G34() {
  if (DEBUGGING(LEVELING)) {
    DEBUG_ECHOLNPGM(">>> G34");
    log_machine_info();
  }

  do { // break out on error

    const int8_t z_auto_align_iterations = parser.intval('I', Z_STEPPER_ALIGN_ITERATIONS);
    if (!WITHIN(z_auto_align_iterations, 1, 30)) {
      SERIAL_ECHOLNPGM("?(I)teration out of bounds (1-30).");
      break;
    }

    const float z_auto_align_accuracy = parser.floatval('T', Z_STEPPER_ALIGN_ACC);
    if (!WITHIN(z_auto_align_accuracy, 0.01f, 1.0f)) {
      SERIAL_ECHOLNPGM("?(T)arget accuracy out of bounds (0.01-1.0).");
      break;
    }

    const float z_auto_align_amplification = parser.floatval('A', Z_STEPPER_ALIGN_AMP);
    if (!WITHIN(ABS(z_auto_align_amplification), 0.5f, 2.0f)) {
      SERIAL_ECHOLNPGM("?(A)mplification out of bounds (0.5-2.0).");
      break;
    }

    // Wait for planner moves to finish!
    planner.synchronize();

    // Disable the leveling matrix before auto-aligning
    #if HAS_LEVELING
      #if ENABLED(RESTORE_LEVELING_AFTER_G34)
        const bool leveling_was_active = planner.leveling_active;
      #endif
      set_bed_leveling_enabled(false);
    #endif

    #if ENABLED(CNC_WORKSPACE_PLANES)
      workspace_plane = PLANE_XY;
    #endif

    // Always home with tool 0 active
    #if HOTENDS > 1
      const uint8_t old_tool_index = active_extruder;
      tool_change(0, true);
    #endif

    #if HAS_DUPLICATION_MODE
      extruder_duplication_enabled = false;
    #endif

    #if BOTH(BLTOUCH, BLTOUCH_HS_MODE)
        // In BLTOUCH HS mode, the probe travels in a deployed state.
        // Users of G34 might have a badly misaligned bed, so raise Z by the
        // length of the deployed pin (BLTOUCH stroke < 7mm)
      #define Z_BASIC_CLEARANCE Z_CLEARANCE_BETWEEN_PROBES + 7.0f
    #else
      #define Z_BASIC_CLEARANCE Z_CLEARANCE_BETWEEN_PROBES
    #endif

    float z_probe = Z_BASIC_CLEARANCE + (G34_MAX_GRADE) * 0.01f * (
      #if ENABLED(Z_TRIPLE_STEPPER_DRIVERS)
         SQRT(_MAX(HYPOT2(z_auto_align_pos[0].x - z_auto_align_pos[0].y, z_auto_align_pos[1].x - z_auto_align_pos[1].y),
                  HYPOT2(z_auto_align_pos[1].x - z_auto_align_pos[1].y, z_auto_align_pos[2].x - z_auto_align_pos[2].y),
                  HYPOT2(z_auto_align_pos[2].x - z_auto_align_pos[2].y, z_auto_align_pos[0].x - z_auto_align_pos[0].y)))
      #else
         HYPOT(z_auto_align_pos[0].x - z_auto_align_pos[0].y, z_auto_align_pos[1].x - z_auto_align_pos[1].y)
      #endif
    );

    // Home before the alignment procedure
    if (!all_axes_known()) home_all_axes();

    // Move the Z coordinate realm towards the positive - dirty trick
    current_position.z -= z_probe * 0.5f;

    float last_z_align_move[Z_STEPPER_COUNT] = ARRAY_N(Z_STEPPER_COUNT, 10000.0f, 10000.0f, 10000.0f),
          z_measured[Z_STEPPER_COUNT] = { 0 },
          z_maxdiff = 0.0f,
          amplification = z_auto_align_amplification;

    const ProbePtRaise raise_after = parser.boolval('E') ? PROBE_PT_STOW : PROBE_PT_RAISE;

    uint8_t iteration;
    bool err_break = false;
    for (iteration = 0; iteration < z_auto_align_iterations; ++iteration) {
      if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("> probing all positions.");

      SERIAL_ECHOLNPAIR("\nITERATION: ", int(iteration + 1));

      // Initialize minimum value
      float z_measured_min = 100000.0f;
      // Probe all positions (one per Z-Stepper)
      for (uint8_t izstepper = 0; izstepper < Z_STEPPER_COUNT; ++izstepper) {
        // iteration odd/even --> downward / upward stepper sequence
        const uint8_t zstepper = (iteration & 1) ? Z_STEPPER_COUNT - 1 - izstepper : izstepper;

        // Safe clearance even on an incline
        if (iteration == 0 || izstepper > 0) do_blocking_move_to_z(z_probe);

        // Probe a Z height for each stepper.
        const float z_probed_height = probe_at_point(z_auto_align_pos[zstepper], raise_after, 0, true);
        if (isnan(z_probed_height)) {
          SERIAL_ECHOLNPGM("Probing failed.");
          err_break = true;
          break;
        }

        // Add height to each value, to provide a more useful target height for
        // the next iteration of probing. This allows adjustments to be made away from the bed.
        z_measured[zstepper] = z_probed_height + Z_CLEARANCE_BETWEEN_PROBES;

        if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR("> Z", int(zstepper + 1), " measured position is ", z_measured[zstepper]);

        // Remember the minimum measurement to calculate the correction later on
        z_measured_min = _MIN(z_measured_min, z_measured[zstepper]);
      } // for (zstepper)

      if (err_break) break;

      // Adapt the next probe clearance height based on the new measurements.
      // Safe_height = lowest distance to bed (= highest measurement) plus highest measured misalignment.
      #if ENABLED(Z_TRIPLE_STEPPER_DRIVERS)
        z_maxdiff = _MAX(ABS(z_measured[0] - z_measured[1]), ABS(z_measured[1] - z_measured[2]), ABS(z_measured[2] - z_measured[0]));
        z_probe = Z_BASIC_CLEARANCE + _MAX(z_measured[0], z_measured[1], z_measured[2]) + z_maxdiff;
      #else
        z_maxdiff = ABS(z_measured[0] - z_measured[1]);
        z_probe = Z_BASIC_CLEARANCE + _MAX(z_measured[0], z_measured[1]) + z_maxdiff;
      #endif

      SERIAL_ECHOPAIR("\n"
        "DIFFERENCE Z1-Z2=", ABS(z_measured[0] - z_measured[1])
        #if ENABLED(Z_TRIPLE_STEPPER_DRIVERS)
          , " Z2-Z3=", ABS(z_measured[1] - z_measured[2])
          , " Z3-Z1=", ABS(z_measured[2] - z_measured[0])
        #endif
      );
      SERIAL_EOL();
      SERIAL_EOL();

      // The following correction actions are to be enabled for select Z-steppers only
      stepper.set_separate_multi_axis(true);

      bool success_break = true;
      // Correct the individual stepper offsets
      for (uint8_t zstepper = 0; zstepper < Z_STEPPER_COUNT; ++zstepper) {
        // Calculate current stepper move
        const float z_align_move = z_measured[zstepper] - z_measured_min,
                    z_align_abs = ABS(z_align_move);

        // Optimize one iterations correction based on the first measurements
        if (z_align_abs > 0.0f) amplification = iteration == 1 ? _MIN(last_z_align_move[zstepper] / z_align_abs, 2.0f) : z_auto_align_amplification;

        // Check for less accuracy compared to last move
        if (last_z_align_move[zstepper] < z_align_abs - 1.0) {
          SERIAL_ECHOLNPGM("Decreasing accuracy detected.");
          err_break = true;
          break;
        }

        // Remember the alignment for the next iteration
        last_z_align_move[zstepper] = z_align_abs;

        // Stop early if all measured points achieve accuracy target
        if (z_align_abs > z_auto_align_accuracy) success_break = false;

        if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR("> Z", int(zstepper + 1), " corrected by ", z_align_move);

        // Lock all steppers except one
        set_all_z_lock(true);
        switch (zstepper) {
          case 0: stepper.set_z_lock(false); break;
          case 1: stepper.set_z2_lock(false); break;
          #if ENABLED(Z_TRIPLE_STEPPER_DRIVERS)
            case 2: stepper.set_z3_lock(false); break;
          #endif
        }

        // Do a move to correct part of the misalignment for the current stepper
        do_blocking_move_to_z(amplification * z_align_move + current_position.z);
      } // for (zstepper)

      // Back to normal stepper operations
      set_all_z_lock(false);
      stepper.set_separate_multi_axis(false);

      if (err_break) break;

      if (success_break) { SERIAL_ECHOLNPGM("Target accuracy achieved."); break; }

    } // for (iteration)

    if (err_break) { SERIAL_ECHOLNPGM("G34 aborted."); break; }

    SERIAL_ECHOLNPAIR("Did ", int(iteration + (iteration != z_auto_align_iterations)), " iterations of ", int(z_auto_align_iterations));
    SERIAL_ECHOLNPAIR_F("Accuracy: ", z_maxdiff);
    SERIAL_EOL();

    // Restore the active tool after homing
    #if HOTENDS > 1
      tool_change(old_tool_index, (
        #if ENABLED(PARKING_EXTRUDER)
          false // Fetch the previous toolhead
        #else
          true
        #endif
      ));
    #endif

    #if HAS_LEVELING && ENABLED(RESTORE_LEVELING_AFTER_G34)
      set_bed_leveling_enabled(leveling_was_active);
    #endif

    // After this operation the z position needs correction
    set_axis_is_not_at_home(Z_AXIS);

    // Stow the probe, as the last call to probe_at_point(...) left
    // the probe deployed if it was successful.
    STOW_PROBE();

    // Home Z after the alignment procedure
    process_subcommands_now_P(PSTR("G28 Z"));

  }while(0);

  if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("<<< G34");
}

/**
 * M422: Z-Stepper automatic alignment parameter selection
 */
void GcodeSuite::M422() {
  const int8_t zstepper = parser.intval('S') - 1;
  if (!WITHIN(zstepper, 0, Z_STEPPER_COUNT - 1)) {
    SERIAL_ECHOLNPGM("?(S) Z-Stepper index invalid.");
    return;
  }

  const xy_pos_t pos = {
    parser.floatval('X', z_auto_align_pos[zstepper].x),
    parser.floatval('Y', z_auto_align_pos[zstepper].y)
  };

  if (!WITHIN(pos.x, X_MIN_POS, X_MAX_POS)) {
    SERIAL_ECHOLNPGM("?(X) out of bounds.");
    return;
  }

  if (!WITHIN(pos.y, Y_MIN_POS, Y_MAX_POS)) {
    SERIAL_ECHOLNPGM("?(Y) out of bounds.");
    return;
  }

  z_auto_align_pos[zstepper] = pos;
}

#endif // Z_STEPPER_AUTO_ALIGN
