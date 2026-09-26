package ru.vitska.powermon.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import ru.vitska.powermon.ble.ConfigState
import ru.vitska.powermon.ble.Chemistries
import ru.vitska.powermon.ble.Link
import ru.vitska.powermon.ble.Micro
import ru.vitska.powermon.model.MonitorViewModel

/**
 * Everything the console can set, grouped the way CLI.md groups it — calibration first,
 * because it is the reason to reach for this screen while standing at the bench with a
 * meter in hand. The rest is set once and left alone.
 *
 * Every control shows what the device currently has, read from `config` rather than
 * remembered from what this app last wrote: the device clamps values, another client can
 * change them, and a stale echo of our own write is worse than no value at all. Each
 * successful setter re-reads it.
 *
 * Two rules from CLI.md section 7 are structural here, not cosmetic:
 *
 *   - Never auto-run calibration. `cal zero i` with a load connected permanently
 *     poisons the offset and the firmware cannot detect it, so every calibration
 *     action goes through a confirmation that states the physical precondition.
 *   - Command output is prose. So a result is shown verbatim rather than parsed --
 *     including the refusal text, which always names a physical cause.
 */
@Composable
fun ConfigureScreen(vm: MonitorViewModel) {
    val busy by vm.busy.collectAsState()
    val link by vm.link.collectAsState()
    val t by vm.telemetry.collectAsState()
    val cfg by vm.config.collectAsState()
    var last by remember { mutableStateOf<String?>(null) }
    var confirm by remember { mutableStateOf<Confirmation?>(null) }

    /** A read-only command: show what it said, change nothing. */
    val run: (String) -> Unit = { cmd ->
        vm.launchCommandWith(cmd) { r ->
            last = if (r == null) {
                "no reply -- timed out"
            } else {
                (if (r.ok) "" else "exit " + r.exit + "\n") + r.text.ifBlank { "(no output)" }
            }
        }
        Unit
    }

    /** A setter: show what it said, then re-read what the device actually holds now. */
    val set: (String) -> Unit = { cmd ->
        vm.launchCommandWith(cmd) { r ->
            last = if (r == null) {
                "no reply -- timed out"
            } else {
                (if (r.ok) "" else "exit " + r.exit + "\n") + r.text.ifBlank { "(applied)" }
            }
            vm.launchRefreshConfig()
        }
        Unit
    }

    val guarded: (Confirmation) -> Unit = { confirm = it }

    LaunchedEffect(link) {
        if (link == Link.Ready && !cfg.supported) vm.refreshConfig()
    }

    Column(
        Modifier.verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (link != Link.Ready) {
            Warn("Not connected. Nothing on this screen can be read or set until a board is.")
        } else if (!cfg.supported) {
            // Degrade honestly rather than showing values we would have to guess at.
            Warn(
                "This firmware has no `config` command, so current values cannot be " +
                    "read. The controls still work; use `options` to see what is set."
            )
        }
        if (busy) Text("command in flight...", style = MaterialTheme.typography.labelMedium)

        last?.let { text ->
            Section("Last response") {
                Text(text, fontFamily = FontFamily.Monospace,
                    style = MaterialTheme.typography.bodySmall)
                Spacer(Modifier.height(6.dp))
                TextButton(onClick = { last = null }) { Text("Dismiss") }
            }
        }

        // ------------------------------------------------------------ calibration

        Section("Calibration") {
            Text(
                "Two zero points, then one known value per channel. The device solves " +
                    "and stores the trims itself — you supply the meter reading.",
                style = MaterialTheme.typography.bodySmall,
            )
            Spacer(Modifier.height(10.dp))

            Text("DEVICE READS NOW", style = MaterialTheme.typography.labelSmall)
            KV("Voltage", t.volts?.let { String.format("%.3f V", it) } ?: "—")
            KV("Current", t.amps?.let { String.format("%.4f A", it) } ?: "—")
            KV("Shunt drop", t.shuntMv?.let { String.format("%.3f mV", it) } ?: "—")
            if (t.saturated) {
                Spacer(Modifier.height(6.dp))
                Text(
                    "Shunt channel is SATURATED — calibrating current against this " +
                        "reading will solve for a range limit, not a measurement.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }

            Spacer(Modifier.height(12.dp))
            HorizontalDivider()
            Spacer(Modifier.height(12.dp))

            Text("TRIMS IN FORCE", style = MaterialTheme.typography.labelSmall)
            KV("Current offset", cfg.long("cal.i_offset_ua")
                ?.let { String.format("%+d uA", it) } ?: "—")
            KV("Current gain", cfg.gainPct("cal.i_gain_ppm")
                ?.let { it + "  (" + cfg.str("cal.i_gain_ppm") + " ppm)" } ?: "—")
            KV("Voltage offset", cfg.long("cal.v_offset_uv")
                ?.let { String.format("%+d uV", it) } ?: "—")
            KV("Voltage gain", cfg.gainPct("cal.v_gain_ppm")
                ?.let { it + "  (" + cfg.str("cal.v_gain_ppm") + " ppm)" } ?: "—")
            KV("Harness path", cfg.milli("shunt.vpath_uohm", 3)?.let { it + " mOhm" } ?: "—")
            KV(
                "Stored in flash",
                when (cfg.bool("cal.stored")) {
                    true -> "yes"
                    false -> "NO — unsaved, run Save"
                    null -> "—"
                },
            )

            Spacer(Modifier.height(12.dp))
            HorizontalDivider()
            Spacer(Modifier.height(12.dp))

            Text("ZERO POINTS", style = MaterialTheme.typography.labelSmall)
            Text(
                "Sets the offset: what the channel reads when the true value is zero. " +
                    "Do these before the known-value points below.",
                style = MaterialTheme.typography.bodySmall,
            )
            Spacer(Modifier.height(8.dp))
            Wrap {
                OutlinedButton(onClick = {
                    guarded(
                        Confirmation(
                            "Record the current zero point?",
                            "THE LOAD MUST BE DISCONNECTED. Running this with current " +
                                "flowing poisons the offset permanently and the firmware " +
                                "cannot detect it. Averages 256 samples, about 35 s.",
                            "cal zero i",
                        )
                    )
                }) { Text("Zero current") }
                OutlinedButton(onClick = {
                    guarded(
                        Confirmation(
                            "Record the voltage zero point?",
                            "VBUS must be tied to GROUND, not merely disconnected — a " +
                                "floating input reads a real voltage and the device will " +
                                "refuse. Averages 256 samples, about 68 s.",
                            "cal zero v",
                        )
                    )
                }) { Text("Zero voltage") }
            }

            Spacer(Modifier.height(12.dp))
            HorizontalDivider()
            Spacer(Modifier.height(12.dp))

            Text("KNOWN VALUES", style = MaterialTheme.typography.labelSmall)
            Text(
                "Enter what your meter reads and the device solves the gain from it. " +
                    "Current must exceed 10 mA and voltage 0.5 V, or there is no slope " +
                    "to solve.",
                style = MaterialTheme.typography.bodySmall,
            )
            Spacer(Modifier.height(8.dp))
            MicroField(
                "Measured current now", "A", "1.959",
                prefill = t.amps?.let { String.format("%.4f", it) },
            ) { v ->
                guarded(
                    Confirmation(
                        "Set the current gain from " + v + " A?",
                        "The meter and the device must be measuring the same current in " +
                            "the same direction — a disagreement on sign is rejected " +
                            "rather than absorbed. Averages 64 samples, about 17 s.",
                        "cal top i " + Micro.amps(v),
                    )
                )
            }
            MicroField(
                "Measured voltage AT REST", "V", "12.44",
                prefill = t.volts?.let { String.format("%.3f", it) },
            ) { v ->
                guarded(
                    Confirmation(
                        "Set the voltage gain from " + v + " V?",
                        "Take this reading with no load. Under load the harness drop " +
                            "makes the solved gain wrong — that is what the harness " +
                            "field below is for. About 17 s.",
                        "cal top v " + Micro.volts(v),
                    )
                )
            }
            MicroField(
                "Terminal voltage UNDER LOAD", "V", "12.10",
                prefill = t.volts?.let { String.format("%.3f", it) },
            ) { v ->
                guarded(
                    Confirmation(
                        "Solve harness resistance from " + v + " V?",
                        "Needs at least 0.5 A flowing, and the reading must be taken at " +
                            "the battery terminals rather than at the board. This is what " +
                            "separates a wiring drop from a gain error.",
                        "cal vpath " + Micro.volts(v),
                    )
                )
            }

            Spacer(Modifier.height(12.dp))
            Wrap {
                AssistChip(onClick = { set("cal save") }, label = { Text("Save") })
                AssistChip(onClick = { vm.launchRefreshConfig() }, label = { Text("Refresh") })
                AssistChip(onClick = { run("curve") }, label = { Text("curve") })
                OutlinedButton(onClick = {
                    guarded(
                        Confirmation(
                            "Erase the stored calibration?",
                            "Both live and stored trims are cleared. The board reverts to " +
                                "nominal scaling until it is calibrated again.",
                            "cal reset",
                        )
                    )
                }) { Text("Erase") }
            }
        }

        // ------------------------------------------------------------ the rest

        Section("Shunt and topology") {
            Text(
                "Get these right before calibrating: a wrong shunt value shows up as a " +
                    "gain outside ±10 %, which the device refuses rather than absorbs.",
                style = MaterialTheme.typography.bodySmall,
            )
            Spacer(Modifier.height(8.dp))
            MicroField(
                "Shunt resistance", "mOhm", "100",
                current = cfg.milli("shunt.uohm", 3),
            ) { set("shunt " + Math.round(it * 1000.0)) }
            Spacer(Modifier.height(8.dp))
            Choice("shunt loc", listOf("p", "n", "single", "auto"),
                cfg.str("shunt.loc")) { set("shunt loc " + it) }
            Choice("sense sign", listOf("normal", "invert"),
                cfg.str("sense.sign")) { set("sense sign " + it) }
            Choice("sense vbuscomp", listOf("none", "add", "sub"),
                cfg.str("sense.vbuscomp")) { set("sense vbuscomp " + it) }
            Choice("sense pgamax", listOf("1", "2", "4", "8"),
                cfg.str("sense.pgamax")) { set("sense pgamax " + it) }
            Spacer(Modifier.height(4.dp))
            KV("Roles", cfg.str("shunt.roles") ?: "—")
            KV("Active range", cfg.str("sense.pga")?.let { "/" + it } ?: "—")
            KV("Autorange", cfg.bool("sense.autorange")?.let { if (it) "on" else "off" } ?: "—")
            Spacer(Modifier.height(8.dp))
            OutlinedButton(onClick = {
                guarded(
                    Confirmation(
                        "Detect topology?",
                        "Needs a load -- the device refuses at zero current. Takes about " +
                            "9 s and overwrites the shunt-location setting.",
                        "detect",
                    )
                )
            }) { Text("detect") }
        }

        Section("Telemetry rates") {
            Text(
                "A group set to off stops without disturbing the others. For a genuine " +
                    "10 Hz fast group the device also needs profile fast -- the default " +
                    "profile tops out at 7.3 Hz.",
                style = MaterialTheme.typography.bodySmall,
            )
            Spacer(Modifier.height(8.dp))
            RateRow("fast", 20, 60_000, cfg.str("stream.fast_ms"), set)
            RateRow("calc", 20, 60_000, cfg.str("stream.calc_ms"), set)
            RateRow("diag", 20, 60_000, cfg.str("stream.diag_ms"), set)
            RateRow("env", 20, 600_000, cfg.str("stream.env_ms"), set)
            Spacer(Modifier.height(4.dp))
            KV(
                "Stream",
                cfg.bool("stream.on")?.let {
                    (if (it) "on" else "off") +
                        (if (cfg.bool("stream.csv") == true) ", CSV" else ", text")
                } ?: "—",
            )
            Spacer(Modifier.height(8.dp))
            Wrap {
                AssistChip(onClick = { set("stream csv") }, label = { Text("stream csv") })
                AssistChip(onClick = { set("stream on") }, label = { Text("stream on") })
                AssistChip(onClick = { set("stream off") }, label = { Text("stream off") })
            }
            Spacer(Modifier.height(8.dp))
            Choice("profile", listOf("continuous", "fast", "triggered"),
                cfg.str("profile")) { set("profile " + it) }
            KV(
                "ADC pair time",
                cfg.long("profile.pair_us")?.let {
                    String.format("%.1f ms  (%.1f Hz ceiling)", it / 1000.0,
                        if (it > 0) 1_000_000.0 / it else 0.0)
                } ?: "—",
            )
        }

        Section("Battery") {
            BatteryPicker(
                currentKey = cfg.str("battery.chem"),
                currentCells = cfg.str("battery.cells"),
                window = cfg.micro("soc.v0_uv", 2)?.let { a ->
                    cfg.micro("soc.v100_uv", 2)?.let { b -> "$a – $b V resting" }
                },
            ) { chem, cells ->
                guarded(
                    Confirmation(
                        "Switch to ${chem.name}" + (cells?.let { ", $it cells" } ?: "") + "?",
                        "Loads that chemistry's voltage curve, endpoints and charge " +
                            "behaviour" + (if (cells == null) ", with the cell count " +
                            "guessed from the present voltage" else "") + ". The charge " +
                            "count restarts from the resting voltage; capacity and " +
                            "calibration are kept.",
                        "battery ${chem.key}" + (cells?.let { " $it" } ?: ""),
                    )
                )
            }
        }

        Section("Fuel gauge") {
            Text(
                "The pack endpoints and gauge behaviour. All of it persists; a rejected " +
                    "value prints the constraint that failed (v0 < v100 <= vfull).",
                style = MaterialTheme.typography.bodySmall,
            )
            Spacer(Modifier.height(8.dp))
            MicroField("Design capacity", "Ah", "44", current = cfg.micro("soc.cap_uah", 1)) {
                set("soc cap " + Micro.ampHours(it))
            }
            MicroField("0 % resting OCV", "V", "11.80", current = cfg.micro("soc.v0_uv", 3)) {
                set("soc v0 " + Micro.volts(it))
            }
            MicroField("100 % resting OCV", "V", "12.75", current = cfg.micro("soc.v100_uv", 3)) {
                set("soc v100 " + Micro.volts(it))
            }
            MicroField("Absorption (full) voltage", "V", "14.40",
                current = cfg.micro("soc.vfull_uv", 3)) {
                set("soc vfull " + Micro.volts(it))
            }
            MicroField("Internal resistance", "mOhm", "8",
                current = cfg.milli("soc.rint_uohm", 3)) {
                set("soc rint " + Math.round(it * 1000.0))
            }
            MicroField("Taper current", "A", "2.2", current = cfg.micro("soc.taper_ua", 3)) {
                set("soc taper " + Micro.amps(it))
            }
            MicroField("Rated discharge current", "A", "2.2",
                current = cfg.micro("soc.irated_ua", 3)) {
                set("soc irated " + Micro.amps(it))
            }
            PlainField("Rest before OCV is trusted", "s", "600",
                current = cfg.str("soc.rest_s")) { set("soc rest " + it) }
            PlainField("Peukert k (Q8; 256 disables)", "q8", "300",
                current = cfg.str("soc.peukert_q8")?.let { q ->
                    q + "  (k " + String.format("%.3f", (q.toIntOrNull() ?: 256) / 256.0) + ")"
                }) { set("soc peukert " + it) }
            PlainField("Learning depth", "per mille", "500",
                current = cfg.str("soc.depth_permille")) { set("soc depth " + it) }
            MicroField("Force SoC", "%", "80",
                current = cfg.long("soc.permille")?.let { String.format("%.1f", it / 10.0) }) {
                set("soc set " + Micro.permille(it))
            }
            Spacer(Modifier.height(4.dp))
            KV("Learned capacity", cfg.micro("soc.learned_uah", 2)?.let { it + " Ah" } ?: "—")
            KV("Integration deadband", cfg.micro("soc.deadband_ua", 3)?.let { it + " A" } ?: "—")
            KV(
                "SoC source",
                when (cfg.bool("soc.voltage_only")) {
                    true -> "VOLTAGE only — no count behind it"
                    false -> "counted"
                    null -> "—"
                },
            )
            Spacer(Modifier.height(8.dp))
            Wrap {
                OutlinedButton(onClick = {
                    guarded(
                        Confirmation(
                            "Declare the pack full?",
                            "This anchors the count at 100 % right now. Only correct if the " +
                                "battery really is at absorption voltage with tapered current.",
                            "soc full",
                        )
                    )
                }) { Text("soc full") }
                OutlinedButton(onClick = {
                    guarded(
                        Confirmation(
                            "Forget the charge count?",
                            "The accumulated count is discarded and SoC re-seeds from " +
                                "voltage, which is the less trustworthy source until the " +
                                "next anchor.",
                            "soc reset",
                        )
                    )
                }) { Text("soc reset") }
            }
        }

        Section("Display") {
            KV(
                "Panel",
                when (cfg.bool("disp.present")) {
                    true -> if (cfg.bool("disp.on") == true) "on" else "blanked"
                    false -> "none fitted"
                    null -> "—"
                },
            )
            KV(
                "Screen",
                cfg.str("disp.screen")?.let { sc ->
                    if (sc == "auto") "auto-cycling " + (cfg.str("disp.screens") ?: "")
                    else "pinned to " + sc
                } ?: "—",
            )
            Spacer(Modifier.height(8.dp))
            Wrap {
                AssistChip(onClick = { set("disp on") }, label = { Text("on") })
                AssistChip(onClick = { set("disp off") }, label = { Text("off") })
                AssistChip(onClick = { set("disp screen auto") }, label = { Text("screen auto") })
                AssistChip(onClick = { set("disp screen 0") }, label = { Text("screen 0") })
            }
            Spacer(Modifier.height(8.dp))
            PlainField("Contrast (1-255; 0x40 default)", "", "64",
                current = cfg.str("disp.contrast")) { set("disp contrast " + it) }
        }

        Section("BLE and pairing") {
            Text(
                "open mode has no pairing at all: anything in range can run every " +
                    "command, calibration included. It is the default and it is a bench " +
                    "setting.",
                style = MaterialTheme.typography.bodySmall,
            )
            Spacer(Modifier.height(8.dp))
            KV("Name", cfg.str("ble.name") ?: "—")
            KV(
                "Links",
                cfg.str("ble.conns")?.let { c ->
                    c + " connected, " + (cfg.str("ble.subs") ?: "?") + " subscribed"
                } ?: "—",
            )
            KV("Bonds", cfg.str("ble.bonds") ?: "—")
            Spacer(Modifier.height(8.dp))
            Choice("ble pair", listOf("open", "bonded"), cfg.str("ble.pair")) { mode ->
                guarded(
                    Confirmation(
                        "Switch pairing to " + mode + "?",
                        if (mode == "bonded") {
                            "This persists and DROPS THE CURRENT LINK. On reconnect the " +
                                "phone will prompt for the six-digit passkey shown on the " +
                                "device OLED."
                        } else {
                            "This persists. Any device in range will then be able to run " +
                                "every command without authentication."
                        },
                        "ble pair " + mode,
                    )
                )
            }
            Spacer(Modifier.height(8.dp))
            PlainField("Fixed passkey (blank for random)", "", "random",
                current = cfg.str("ble.passkey")) {
                set("ble passkey " + it.ifBlank { "random" })
            }
            Wrap {
                AssistChip(onClick = { run("ble bonds") }, label = { Text("bonds") })
                OutlinedButton(onClick = {
                    guarded(
                        Confirmation(
                            "Forget all bonds?",
                            "Every bonded phone must pair again, and the current link is " +
                                "dropped.",
                            "ble unpair",
                        )
                    )
                }) { Text("unpair") }
            }
        }

        Section("Read state") {
            // The prose overviews, for the things `config` deliberately does not carry:
            // history, statistics, and anything a person reads rather than a program.
            Wrap {
                listOf(
                    "ver", "options", "soc", "curve", "cal", "shunt", "sensors",
                    "stream", "profile", "disp", "ble", "scan", "stats", "read", "env",
                    "config",
                ).forEach { c -> AssistChip(onClick = { run(c) }, label = { Text(c) }) }
            }
        }

        Spacer(Modifier.height(24.dp))
    }

    confirm?.let { c ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            title = { Text(c.title) },
            text = { Text(c.body) },
            confirmButton = {
                Button(onClick = { confirm = null; set(c.command) }) { Text("Run") }
            },
            dismissButton = {
                TextButton(onClick = { confirm = null }) { Text("Cancel") }
            },
        )
    }
}

private data class Confirmation(val title: String, val body: String, val command: String)

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun Wrap(content: @Composable () -> Unit) {
    FlowRow(
        Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) { content() }
}

/** Options for one setting, with the device's current choice marked. */
@Composable
private fun Choice(
    label: String,
    options: List<String>,
    current: String?,
    onPick: (String) -> Unit,
) {
    Column(Modifier.fillMaxWidth()) {
        Text(label, style = MaterialTheme.typography.labelMedium)
        Wrap {
            options.forEach { o ->
                FilterChip(
                    selected = current == o,
                    onClick = { onPick(o) },
                    label = { Text(o) },
                )
            }
        }
    }
}

@Composable
private fun RateRow(
    group: String,
    min: Int,
    max: Int,
    current: String?,
    set: (String) -> Unit,
) {
    var text by remember { mutableStateOf("") }
    val v = text.toIntOrNull()
    Column(Modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = text,
                onValueChange = { s -> text = s.filter { it.isDigit() } },
                label = { Text("stream " + group + " (ms)") },
                // The placeholder is the device's value, so an untouched field is not
                // silently suggesting a default the board does not have.
                placeholder = { Text(current ?: "—") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                modifier = Modifier.weight(1f),
            )
            Spacer(Modifier.width(8.dp))
            Button(
                onClick = { if (v != null && v in min..max) set("stream " + group + " " + v) },
                enabled = v != null && v in min..max,
            ) { Text("Set") }
            TextButton(onClick = { set("stream " + group + " off") }) { Text("Off") }
        }
        Current(current, if (current == "0") "off" else current?.let { it + " ms" })
    }
}

/**
 * A field in human units; the lambda turns the value into a micro-unit command.
 *
 * [current] is what the device holds, shown under the field. [prefill] offers a live
 * reading as a starting point — used for calibration, where the point is to type over it.
 */
@Composable
private fun MicroField(
    label: String,
    unit: String,
    hint: String,
    current: String? = null,
    prefill: String? = null,
    onSet: (Double) -> Unit,
) {
    var text by remember { mutableStateOf("") }
    val v = text.replace(',', '.').toDoubleOrNull()
    Column(Modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = text,
                onValueChange = { text = it },
                label = { Text(if (unit.isBlank()) label else label + " (" + unit + ")") },
                placeholder = { Text(current ?: hint) },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                modifier = Modifier.weight(1f),
            )
            Spacer(Modifier.width(8.dp))
            Button(onClick = { v?.let(onSet) }, enabled = v != null) { Text("Set") }
        }
        Current(current, current?.let { it + " " + unit })
        if (prefill != null && text.isEmpty()) {
            TextButton(onClick = { text = prefill }) { Text("use device reading " + prefill) }
        }
    }
}

/** A field whose value is already in the units the command wants. */
@Composable
private fun PlainField(
    label: String,
    unit: String,
    hint: String,
    current: String? = null,
    onSet: (String) -> Unit,
) {
    var text by remember { mutableStateOf("") }
    Column(Modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = text,
                onValueChange = { text = it },
                label = { Text(if (unit.isBlank()) label else label + " (" + unit + ")") },
                placeholder = { Text(current ?: hint) },
                singleLine = true,
                modifier = Modifier.weight(1f),
            )
            Spacer(Modifier.width(8.dp))
            Button(onClick = { onSet(text) }, enabled = text.isNotBlank()) { Text("Set") }
        }
        Current(current, current)
    }
}

/**
 * Chemistry chips plus a cell count. Nothing is sent until Apply, because applying
 * restarts the charge count -- picking a chip by accident must not do that.
 */
@Composable
private fun BatteryPicker(
    currentKey: String?,
    currentCells: String?,
    window: String?,
    onApply: (Chemistries.Chem, Int?) -> Unit,
) {
    var picked by remember(currentKey) { mutableStateOf(Chemistries.byKey(currentKey)) }
    var cellsText by remember(currentCells) { mutableStateOf(currentCells ?: "") }
    val cells = cellsText.toIntOrNull()?.takeIf { it in 1..32 }
    val current = Chemistries.byKey(currentKey)

    Column(Modifier.fillMaxWidth()) {
        Text(
            "Sets the voltage-to-SoC curve, the 0 %/100 %/full voltages and the charge " +
                "behaviour for this chemistry. Every Fuel gauge value below can still be " +
                "fine-tuned afterwards.",
            style = MaterialTheme.typography.bodySmall,
        )
        Spacer(Modifier.height(8.dp))
        Wrap {
            Chemistries.ALL.forEach { c ->
                FilterChip(
                    selected = picked == c,
                    onClick = { picked = c },
                    label = { Text(c.short) },
                )
            }
        }
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = cellsText,
                onValueChange = { s -> cellsText = s.filter { it.isDigit() }.take(2) },
                label = { Text("cells in series") },
                placeholder = { Text("guess") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                modifier = Modifier.weight(1f),
            )
            Spacer(Modifier.width(8.dp))
            Button(
                onClick = { picked?.let { onApply(it, cells) } },
                enabled = picked != null && (cellsText.isEmpty() || cells != null),
            ) { Text("Apply") }
        }
        if (current != null) {
            Text(
                "on the device: ${current.name}, ${currentCells ?: "?"} cells" +
                    (window?.let { " — $it" } ?: ""),
                style = MaterialTheme.typography.labelSmall,
                fontFamily = FontFamily.Monospace,
            )
        } else if (currentKey == null) {
            Text(
                "This firmware does not report a chemistry (before 0.8.0).",
                style = MaterialTheme.typography.labelSmall,
            )
        }
    }
}

/** "on the device: X", or nothing at all when the device has not told us. */
@Composable
private fun Current(current: String?, rendered: String?) {
    if (current != null && rendered != null) {
        Text(
            "on the device: " + rendered,
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
        )
    }
}
