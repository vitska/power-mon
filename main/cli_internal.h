/*
 * cli_internal.h — what the console's command files share with each other.
 *
 * cli.c owns the framing, both transports and the command table. The commands
 * themselves live in cli_sense.c, cli_stream.c, cli_gauge.c, cli_cal.c and
 * cli_report.c, split out when one file reached a quarter of the firmware.
 *
 * NOT A PUBLIC HEADER. cli.h is what the rest of the firmware includes; everything
 * here is between these five files, and the narrowness of it is the point: three
 * helpers and a context pointer, plus the command entry points the table needs. A
 * command that wants more than this from another command file is a command that has
 * been put in the wrong file.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_ctx.h"
#include "ina219.h"

/** The bring-up context: sensors, the environmental probe, stream state. Set once by
 *  cli_start(), read everywhere. */
extern app_ctx_t *cli_ctx;

/** True, with a message printed, when the sensors did not come up. The console runs
 *  regardless so that `scan` can diagnose exactly that, so every command that touches
 *  hardware asks first. */
bool cli_no_sensors(void);

/** The current-channel device, or NULL with a message saying which of the two reasons
 *  it is missing. Roles resolve only once a load has been seen (DESIGN.md 2.10.6). */
ina219_handle_t cli_current_dev_or_complain(void);

/**
 * Captures the calibration out of the drivers and writes it to flash, reporting either
 * outcome. Every successful calibration point calls it: the alternative is to measure
 * and then remember to type `cal save`, and the one time that is forgotten is the time
 * the bench session has to be repeated.
 */
void cli_cal_autosave(void);

/**
 * Solves a gain trim from one measurement against a reference, printing both and the
 * ppm it arrived at, or saying why it cannot. Shared because `curve` and the guided
 * `cal` are the same arithmetic reached two ways -- one value typed in, one averaged
 * over a run of samples -- and they must not drift apart.
 */
bool cli_solve_gain(int64_t measured, int64_t reference, uint32_t old_ppm,
                    uint32_t *new_ppm);

/* --- the command table's entry points ------------------------------------------ */

int cmd_read(int argc, char **argv);
int cmd_raw(int argc, char **argv);
int cmd_sensors(int argc, char **argv);
int cmd_detect(int argc, char **argv);
int cmd_zero(int argc, char **argv);
int cmd_shunt(int argc, char **argv);
int cmd_gain(int argc, char **argv);
int cmd_offset(int argc, char **argv);
int cmd_pga(int argc, char **argv);
int cmd_profile(int argc, char **argv);
int cmd_sense(int argc, char **argv);
int cmd_scan(int argc, char **argv);
int cmd_curve(int argc, char **argv);

int cmd_stream(int argc, char **argv);
int cmd_stats(int argc, char **argv);
int cmd_mon(int argc, char **argv);
int cmd_env(int argc, char **argv);
int cmd_ver(int argc, char **argv);

int cmd_soc(int argc, char **argv);
int cmd_battery(int argc, char **argv);
int cmd_hist(int argc, char **argv);

int cmd_cal(int argc, char **argv);

int cmd_disp(int argc, char **argv);
int cmd_ble(int argc, char **argv);
int cmd_config(int argc, char **argv);
int cmd_options(int argc, char **argv);
int cmd_ota(int argc, char **argv);
int cmd_reboot(int argc, char **argv);
