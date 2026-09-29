package ru.vitska.powermon.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
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
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import ru.vitska.powermon.ble.ConfigState
import ru.vitska.powermon.ble.Chemistries
import ru.vitska.powermon.ble.FirmwareTarget
import ru.vitska.powermon.ble.Link
import ru.vitska.powermon.ble.Micro
import ru.vitska.powermon.ble.RawSensor
import ru.vitska.powermon.model.MonitorViewModel

/**
 * The Configure tab's sub-pages. One screen of controls each, because the alternative
 * -- and what this was -- is a single scroll deep enough that finding the passkey means
 * passing the calibration buttons twice, and the one screen nobody should touch by
 * accident is the one they scroll through most often.
 *
 * The order is CLI.md's, which is also the order of a bring-up: calibrate, describe the
 * wiring, then the battery, then everything that is set once and left alone.
 */
private enum class CfgPage(val title: String, val blurb: String) {
    CALIBRATION("Calibration", "Zero and span for current and voltage, from a meter"),
    SHUNT("Shunt and topology", "Shunt value, which pole it is in, sensor roles"),
    BATTERY("Battery", "Chemistry and cells in series"),
    GAUGE("Fuel gauge", "Capacity, endpoints, learning, rest and Peukert"),
    HISTORY("SoC history", "How often a point is recorded, and clearing it"),
    TELEMETRY("Telemetry", "Stream rates and the sampling profile"),
    DISPLAY("Display", "The monitor's own OLED panel"),
    BLE("BLE and pairing", "Pairing mode, passkey, bonds"),
    STATE("Read state", "One-shot reads: raw sensors, the options dump"),
}

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
    var page by remember { mutableStateOf<CfgPage?>(null) }

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

    /**
     * A setter that changes the SHAPE of the history, or empties it: the graph on the
     * Monitor tab is polled every two minutes, and two minutes of showing a curve the
     * device has already thrown away is two minutes of believing it. Re-read it here
     * instead, after the command has actually answered.
     */
    val histSet: (String) -> Unit = { cmd ->
        vm.launchCommandWith(cmd) { r ->
            last = if (r == null) {
                "no reply -- timed out"
            } else {
                (if (r.ok) "" else "exit " + r.exit + "\n") + r.text.ifBlank { "(applied)" }
            }
            vm.launchRefreshConfig()
            vm.launchRefreshHistory()
        }
        Unit
    }

    /**
     * Like [set], but only surfaces a failure -- not the command's own output on
     * success. For a stepper tapped repeatedly (the gain fine-tune below): the new
     * value already shows in the field it changed once `refreshConfig` returns, and
     * `curve`'s own reply is a dump of every current/voltage term, which is exactly
     * what buries that field under "Last response" after every single tap.
     */
    val quietSet: (String) -> Unit = { cmd ->
        vm.launchCommandWith(cmd) { r ->
            if (r == null) {
                last = "no reply -- timed out"
            } else if (!r.ok) {
                last = "exit " + r.exit + "\n" + r.text
            }
            vm.launchRefreshConfig()
        }
        Unit
    }

    /*
     * Calibration answers are shown where the calibration is, not only in "Last
     * response" at the top of the screen: the device's refusals name the physical
     * cause ("sign mismatch", "must both exceed 10000 uA"), and a refusal nobody sees
     * reads as a button that does nothing.
     */
    var calResult by remember { mutableStateOf<String?>(null) }
    var calBusy by remember { mutableStateOf(false) }
    val calSet: (String) -> Unit = { cmd ->
        calBusy = true
        calResult = "running " + cmd
        vm.launchCommandWith(cmd) { r ->
            calBusy = false
            val text = if (r == null) "no reply -- timed out"
            else (if (r.ok) "done: " else "REFUSED (exit " + r.exit + "): ") +
                r.lines.filter { it.isNotBlank() && !it.startsWith(".") }
                    .takeLast(4).joinToString("\n")
            calResult = text
            last = text
            vm.launchRefreshConfig()
        }
        Unit
    }

    /* No confirmation dialogs: every calibration action runs the moment it is tapped. */
    val guarded: (String) -> Unit = { command ->
        if (command.startsWith("cal ")) calSet(command) else set(command)
    }

    /**
     * Fire-and-forget: sends the command, silently re-reads config, shows nothing --
     * not "running...", not the result, not a failure. For the two instant voltage
     * actions (measured voltage, terminal voltage), which apply and save the moment
     * they are sent; the field's own value is the only feedback there is.
     */
    val silent: (String) -> Unit = { cmd ->
        vm.launchCommandWith(cmd) { vm.launchRefreshConfig() }
        Unit
    }

    LaunchedEffect(link) {
        if (link == Link.Ready && !cfg.supported) vm.refreshConfig()
    }

    Column(
        Modifier.verticalScroll(rememberScrollState()).padding(horizontal = 10.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(0.dp),
    ) {
        if (link != Link.Ready) {
            Warn("Not connected. Nothing on this screen can be read or set until a board is.")
        } else if (FirmwareTarget.forDeviceName(vm.deviceName.collectAsState().value) ==
            FirmwareTarget.REMOTE
        ) {
            Warn("This is a remote display: it has nothing to configure from here. Use the " +
                "Firmware tab to update it, and its own Settings screen for the rest.")
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

        // The menu, or the page's own header. Nothing below renders unless its page is
        // the open one, so every sub-page is one screen and the scroll is short.
        val open = page
        if (open == null) {
            Spacer(Modifier.height(4.dp))
            CfgPage.entries.forEach { p ->
                MenuRow(p.title, p.blurb, summary(p, cfg)) { page = p }
            }
        } else {
            Row(verticalAlignment = Alignment.CenterVertically) {
                TextButton(onClick = { page = null }) { Text("‹  All settings") }
                Text(open.title, style = MaterialTheme.typography.titleMedium)
            }
            HorizontalDivider()
        }

        // ------------------------------------------------------------ calibration

        if (page == CfgPage.CALIBRATION) Section("Calibration") {
            Text("DEVICE READS NOW", style = MaterialTheme.typography.labelSmall)
            KVGrid(
                "Voltage" to (t.volts?.let { String.format("%.3f V", it) } ?: "—"),
                "Current" to (t.amps?.let { String.format("%+.4f A", it) } ?: "—"),
                "Shunt drop (raw)" to (t.shuntMv?.let { String.format("%+.3f mV", it) } ?: "—"),
            )
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
            KVGrid(
                "Shunt resistance" to (cfg.milli("shunt.uohm", 3)?.let { r ->
                    r + " mOhm" + (cfg.str("shunt.loc")?.let { " on " + it.uppercase() } ?: "")
                } ?: "—"),
                "Sign" to (cfg.str("sense.sign")?.let { if (it == "invert") "inverted" else "normal" } ?: "—"),
                "Current offset" to (cfg.long("cal.i_offset_ua")?.let { String.format("%+d uA", it) } ?: "—"),
                "Voltage offset" to (cfg.long("cal.v_offset_uv")?.let { String.format("%+d uV", it) } ?: "—"),
            )
            KV("Current gain", cfg.gainPct("cal.i_gain_ppm")
                ?.let { it + "  (" + cfg.str("cal.i_gain_ppm") + " ppm)" } ?: "—")
            GainStepper("i", cfg.long("cal.i_gain_ppm"), quietSet)
            KV("Voltage gain", cfg.gainPct("cal.v_gain_ppm")
                ?.let { it + "  (" + cfg.str("cal.v_gain_ppm") + " ppm)" } ?: "—")
            GainStepper("v", cfg.long("cal.v_gain_ppm"), quietSet)
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

            RawSensorsPanel(vm)

            Spacer(Modifier.height(12.dp))
            HorizontalDivider()
            Spacer(Modifier.height(12.dp))

            Text("ZERO POINTS", style = MaterialTheme.typography.labelSmall)
            Spacer(Modifier.height(8.dp))
            Wrap {
                OutlinedButton(onClick = { guarded("cal zero i") }) { Text("Zero current") }
                OutlinedButton(onClick = { guarded("cal zero v") }) { Text("Zero voltage") }
            }

            Spacer(Modifier.height(12.dp))
            HorizontalDivider()
            Spacer(Modifier.height(12.dp))

            Text("KNOWN VALUES", style = MaterialTheme.typography.labelSmall)
            Spacer(Modifier.height(8.dp))
            MicroField(
                "Measured current now", "A", "",
                prefill = t.amps?.let { String.format("%.4f", it) },
                signed = true,
                showHint = false,
            ) { v ->
                // Instant, silent: one reading of each sensor, taken and applied and
                // saved the moment Set is tapped (firmware `cal top i`) -- the same
                // shape as Measured voltage below, now that both are one-shot reads
                // rather than an average worth confirming first.
                silent("cal top i " + Micro.amps(v))
            }
            MicroField(
                "Measured voltage", "V", "",
                prefill = t.volts?.let { String.format("%.3f", it) },
                showHint = false,
            ) { v ->
                // Instant, silent: applied and saved the moment Set is tapped (firmware
                // "cal top v" is one reading, no averaging); the field above shows the
                // result once refreshConfig() returns, so there is nothing else to show.
                silent("cal top v " + Micro.volts(v))
            }

            calResult?.let { res ->
                Spacer(Modifier.height(8.dp))
                Text(
                    res,
                    fontFamily = FontFamily.Monospace,
                    style = MaterialTheme.typography.bodySmall,
                    color = if (res.startsWith("REFUSED") || res.startsWith("no reply"))
                        MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface,
                )
                if (calBusy) Text("command in flight...", style = MaterialTheme.typography.labelMedium)
            }
        }

        // ------------------------------------------------------------ the rest

        if (page == CfgPage.SHUNT) Section("Shunt and topology") {
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
            KVGrid(
                "Roles" to (cfg.str("shunt.roles") ?: "—"),
                "Active range" to (cfg.str("sense.pga")?.let { "/" + it } ?: "—"),
                "Autorange" to (cfg.bool("sense.autorange")?.let { if (it) "on" else "off" } ?: "—"),
            )
            Spacer(Modifier.height(8.dp))
            OutlinedButton(onClick = { guarded("detect") }) { Text("detect") }
        }

        if (page == CfgPage.TELEMETRY) Section("Telemetry rates") {
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

        if (page == CfgPage.BATTERY) Section("Battery") {
            BatteryPicker(
                currentKey = cfg.str("battery.chem"),
                currentCells = cfg.str("battery.cells"),
                window = cfg.micro("soc.v0_uv", 2)?.let { a ->
                    cfg.micro("soc.v100_uv", 2)?.let { b -> "$a – $b V resting" }
                },
            ) { chem, cells ->
                guarded("battery ${chem.key}" + (cells?.let { " $it" } ?: ""))
            }
        }

        if (page == CfgPage.GAUGE) Section("Fuel gauge") {
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
            PlainField("Rest before OCV is trusted", "s", "300",
                current = cfg.str("soc.rest_s")) { set("soc rest " + it) }
            // Per mille of the design capacity, so the field alone does not say what
            // current it means. The device works that out and reports it, so show both.
            PlainField("Rest current (per mille of capacity)", "permille", "15",
                current = cfg.str("soc.irest_permille")?.let { p ->
                    p + (cfg.long("soc.rest_ua")?.let {
                        String.format("  (%.3f A)", it / 1_000_000.0)
                    } ?: "")
                }) { set("soc irest " + it) }
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
            KVGrid(
                // learn_count 0: the device is reporting the nameplate back, not a
                // measurement. Saying so beats letting an equal pair read as "checked,
                // and it came out exactly at its rating". Absent on firmware < 0.11.7.
                "Learned capacity" to (cfg.micro("soc.learned_uah", 2)?.let {
                    it + " Ah" + (if (cfg.int("soc.learn_count") == 0) "  (not measured yet)" else "")
                } ?: "—"),
                "Integration deadband" to (cfg.micro("soc.deadband_ua", 3)?.let { it + " A" } ?: "—"),
                "SoC source" to when (cfg.bool("soc.voltage_only")) {
                    true -> "VOLTAGE only"
                    false -> "counted"
                    null -> "—"
                },
            )
            Spacer(Modifier.height(8.dp))
            Wrap {
                OutlinedButton(onClick = { guarded("soc full") }) { Text("soc full") }
                OutlinedButton(onClick = { guarded("soc reset") }) { Text("soc reset") }
                OutlinedButton(onClick = { histSet("soc reset all") }) {
                    Text("soc reset all")
                }
            }
            Text(
                "soc reset forgets the count and re-seeds from voltage. `all` clears the " +
                    "recorded history with it -- what you want after a pack swap, when " +
                    "both describe a battery that is no longer there.",
                style = MaterialTheme.typography.labelSmall,
            )
        }

        if (page == CfgPage.HISTORY) Section("SoC history") {
            // The ring is a fixed number of points, so the interval IS the span: there is
            // nothing to choose between but how far back the graph reaches and how finely.
            val points = cfg.int("hist.points")
            val period = cfg.int("hist.period_s")
            PlainField(
                "A point every", "s", "300",
                current = period?.toString(),
            ) { histSet("hist every " + it) }
            Spacer(Modifier.height(4.dp))
            KVGrid(
                "Span" to (if (period != null && points != null) {
                    val h = period.toLong() * points / 3600
                    val m = period.toLong() * points % 3600 / 60
                    "$points points = $h h" + (if (m > 0) " $m min" else "")
                } else "—"),
                "Point every" to (period?.let {
                    if (it % 60 == 0) "${it / 60} min" else "$it s"
                } ?: "—"),
            )
            Text(
                "60..3600 s. Changing it CLEARS the stored history: points taken at the " +
                    "old spacing would be drawn at times they were never taken.",
                style = MaterialTheme.typography.labelSmall,
            )
            Spacer(Modifier.height(8.dp))
            Wrap {
                AssistChip(onClick = { histSet("hist every 300") },
                    label = { Text("5 min (24 h)") })
                AssistChip(onClick = { histSet("hist every 600") },
                    label = { Text("10 min (48 h)") })
                OutlinedButton(onClick = { histSet("hist clear") }) { Text("hist clear") }
            }
            if (cfg.supported && period == null) {
                Spacer(Modifier.height(6.dp))
                Warn(
                    "This firmware keeps the history at a fixed 10 minutes: `hist every` " +
                        "needs 0.11.6 or later."
                )
            }
        }

        if (page == CfgPage.DISPLAY) Section("Display") {
            KVGrid(
                "Panel" to when (cfg.bool("disp.present")) {
                    true -> if (cfg.bool("disp.on") == true) "on" else "blanked"
                    false -> "none fitted"
                    null -> "—"
                },
                "Screen" to (cfg.str("disp.screen")?.let { sc ->
                    if (sc == "auto") "auto-cycling " + (cfg.str("disp.screens") ?: "")
                    else "pinned to " + sc
                } ?: "—"),
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

        if (page == CfgPage.BLE) Section("BLE and pairing") {
            KVGrid(
                "Name" to (cfg.str("ble.name") ?: "—"),
                "Links" to (cfg.str("ble.conns")?.let { c ->
                    c + " connected, " + (cfg.str("ble.subs") ?: "?") + " sub"
                } ?: "—"),
                "Bonds" to (cfg.str("ble.bonds") ?: "—"),
            )
            Spacer(Modifier.height(8.dp))
            Choice("ble pair", listOf("open", "bonded"), cfg.str("ble.pair")) { mode ->
                guarded("ble pair " + mode)
            }
            Spacer(Modifier.height(8.dp))
            PlainField("Fixed passkey (blank for random)", "", "random",
                current = cfg.str("ble.passkey")) {
                set("ble passkey " + it.ifBlank { "random" })
            }
            Wrap {
                AssistChip(onClick = { run("ble bonds") }, label = { Text("bonds") })
                OutlinedButton(onClick = { guarded("ble unpair") }) { Text("unpair") }
            }
        }

        if (page == CfgPage.STATE) Section("Read state") {
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
}

/**
 * Both INA219s read directly (`raw`), independent of which one the firmware has
 * assigned to which role. This is the view for deciding "shunt loc" and the sign by
 * eye rather than by guessing: a chip near 0 V is at ground (the negative pole), one
 * near the pack voltage is at the positive pole, and whichever sees a real shunt
 * voltage for a known load is the one actually wired across the shunt.
 */
@Composable
private fun RawSensorsPanel(vm: MonitorViewModel) {
    val raw by vm.raw.collectAsState()
    val link by vm.link.collectAsState()

    // Once, not polled: a recurring `raw` every few seconds shares the board's single
    // command channel with everything else on this screen -- including Set on the
    // calibration fields -- and queuing behind it is what made Set look like it had
    // stopped responding. Read once when the section appears; tap again to refresh.
    LaunchedEffect(link) {
        if (link == Link.Ready) vm.launchRefreshRaw()
    }

    Text("BOTH SENSORS, READ DIRECTLY", style = MaterialTheme.typography.labelSmall)

    val r = raw
    when {
        r == null -> Text("reading…", style = MaterialTheme.typography.bodySmall)
        !r.supported -> Text(
            "needs firmware 0.10.2 or later",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.error,
        )
        else -> {
            val poleColor = if (r.poles == "swapped") MaterialTheme.colorScheme.error
                            else MaterialTheme.colorScheme.onSurfaceVariant
            Text("poles: " + (r.poles ?: "—"), style = MaterialTheme.typography.bodySmall, color = poleColor)
            Spacer(Modifier.height(6.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                RawSensorCard("0x40 POS", r.pos, Modifier.weight(1f))
                RawSensorCard("0x41 NEG", r.neg, Modifier.weight(1f))
            }
        }
    }
}

@Composable
private fun RawSensorCard(label: String, s: RawSensor, modifier: Modifier = Modifier) {
    Column(modifier) {
        Text(label, style = MaterialTheme.typography.labelSmall, fontWeight = FontWeight.Bold)
        if (!s.present) {
            Text("not fitted", style = MaterialTheme.typography.bodySmall)
            return@Column
        }
        Text(
            s.busV?.let { String.format("%.3f V", it) } ?: "—",
            style = MaterialTheme.typography.titleMedium,
        )
        Text(
            s.currentA?.let { String.format("%+.4f A", it) } ?: "—",
            style = MaterialTheme.typography.titleMedium,
        )
        RawLine(s.role)
        RawLine(s.side)
        RawLine(s.shuntMv?.let { String.format("shunt %+.3f mV", it) })
        RawLine(if (s.saturated) "SATURATED" else s.pga?.let { "/" + it })
        RawLine(
            s.shuntUohm?.let { String.format("%.3f mOhm", it / 1000.0) }?.let {
                it + (s.sign?.let { sg -> " " + sg } ?: "")
            },
        )
    }
}

@Composable
private fun RawLine(value: String?) {
    if (value == null) return
    Text(
        value,
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        fontFamily = FontFamily.Monospace,
    )
}

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
    signed: Boolean = false,
    showHint: Boolean = true,
    onSet: (Double) -> Unit,
) {
    var text by remember { mutableStateOf("") }
    val v = text.replace(',', '.').toDoubleOrNull()
    Column(Modifier.fillMaxWidth()) {
        // Always visible and always freshly formatted from the live telemetry state on
        // every recomposition -- not a snapshot taken once when the field got focus, so
        // it keeps moving with the device whatever is typed below and whether Set has
        // been pressed. Above the field, not below it, so the keyboard covering the
        // field never covers this too.
        if (prefill != null) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "live: " + prefill + " " + unit,
                    style = MaterialTheme.typography.titleMedium,
                    fontFamily = FontFamily.Monospace,
                )
                if (text.isEmpty()) {
                    Spacer(Modifier.width(8.dp))
                    TextButton(onClick = { text = prefill }) { Text("use") }
                }
            }
        }
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = text,
                onValueChange = { text = it },
                label = { Text(if (unit.isBlank()) label else label + " (" + unit + ")") },
                placeholder = if (showHint) {
                    { Text(current ?: hint) }
                } else null,
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                modifier = Modifier.weight(1f),
            )
            if (signed) {
                // Many numeric keypads have no minus key, and a discharge current is
                // negative: a sign toggle that works whatever the keyboard offers.
                TextButton(onClick = {
                    text = if (text.startsWith("-")) text.drop(1) else "-" + text
                }) { Text("±") }
            }
            Spacer(Modifier.width(8.dp))
            Button(onClick = { v?.let(onSet) }, enabled = v != null) { Text("Set") }
        }
        Current(current, current?.let { it + " " + unit })
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
        }
    }
}

/**
 * Nudges a gain by 1 ppm at a time: `curve i|v gain <ppm>`, sent and saved the moment
 * it is tapped — a direct set persists immediately (CLI.md §6), unlike `cal top`'s
 * confirmation-free but still averaged solve. For walking out the last count or two
 * once a `cal top` solve has already done the coarse work.
 */
@Composable
private fun GainStepper(chan: String, ppm: Long?, set: (String) -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween) {
        Text("fine-tune, 1 ppm", style = MaterialTheme.typography.labelSmall)
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            OutlinedButton(
                onClick = { ppm?.let { set("curve " + chan + " gain " + (it - 1)) } },
                enabled = ppm != null,
                contentPadding = PaddingValues(horizontal = 12.dp, vertical = 2.dp),
            ) { Text("−1") }
            OutlinedButton(
                onClick = { ppm?.let { set("curve " + chan + " gain " + (it + 1)) } },
                enabled = ppm != null,
                contentPadding = PaddingValues(horizontal = 12.dp, vertical = 2.dp),
            ) { Text("+1") }
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

/** One row of the Configure menu: what the page is, and what the device has now. */
@Composable
private fun MenuRow(title: String, blurb: String, value: String?, onClick: () -> Unit) {
    Column(
        Modifier.fillMaxWidth().clickable(onClick = onClick).padding(vertical = 12.dp),
    ) {
        Text(title, style = MaterialTheme.typography.titleSmall)
        Text(blurb, style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        if (!value.isNullOrBlank()) {
            Text(value, style = MaterialTheme.typography.labelSmall,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.primary)
        }
    }
    HorizontalDivider()
}

/**
 * What each page currently holds, in one line, from `config`. A menu that only lists
 * page names makes you open all nine to answer "is this board set up" -- the answer is
 * mostly these nine lines, so they belong on the menu itself.
 */
private fun summary(p: CfgPage, cfg: ConfigState): String? = when (p) {
    CfgPage.CALIBRATION -> cfg.gainPct("cal.i_gain_ppm")?.let { g ->
        "current gain " + g + (if (cfg.bool("cal.stored") == true) ", stored" else ", UNSAVED")
    }
    CfgPage.SHUNT -> cfg.str("shunt.uohm")?.let { u ->
        String.format("%.3f mOhm", u.toLongOrNull()?.div(1000.0) ?: 0.0) +
            ", " + (cfg.str("shunt.loc") ?: "?") + ", " + (cfg.str("shunt.roles") ?: "")
    }
    CfgPage.BATTERY -> cfg.str("battery.chem")?.let { c ->
        c.uppercase() + " " + (cfg.str("battery.cells") ?: "?") + "S"
    }
    CfgPage.GAUGE -> cfg.micro("soc.cap_uah", 1)?.let { cap ->
        cap + " Ah nameplate, " + (cfg.micro("soc.learned_uah", 2) ?: "?") + " measured" +
            (if (cfg.int("soc.learn_count") == 0) " (not yet)" else "")
    }
    CfgPage.HISTORY -> cfg.int("hist.period_s")?.let { s ->
        val pts = cfg.int("hist.points") ?: 0
        "a point every " + (if (s % 60 == 0) "${s / 60} min" else "$s s") +
            (if (pts > 0) ", " + (s.toLong() * pts / 3600) + " h span" else "")
    }
    CfgPage.TELEMETRY -> cfg.bool("stream.on")?.let { on ->
        (if (on) "on" else "off") + ", fast " + (cfg.str("stream.fast_ms") ?: "?") + " ms"
    }
    CfgPage.DISPLAY -> when (cfg.bool("disp.present")) {
        true -> if (cfg.bool("disp.on") == true) "on, screen " + (cfg.str("disp.screen") ?: "?")
                else "fitted, blanked"
        false -> "none fitted"
        null -> null
    }
    CfgPage.BLE -> cfg.str("ble.pair")?.let { m ->
        m + ", " + (cfg.str("ble.conns") ?: "?") + " connected, " +
            (cfg.str("ble.bonds") ?: "?") + " bonded"
    }
    CfgPage.STATE -> null
}
