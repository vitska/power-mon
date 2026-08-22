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
import ru.vitska.powermon.ble.Link
import ru.vitska.powermon.ble.Micro
import ru.vitska.powermon.model.MonitorViewModel

/**
 * Everything the console can set, grouped the way CLI.md groups it — calibration first,
 * because it is the reason to reach for this screen while standing at the bench with a
 * meter in hand. The rest is set once and left alone.
 *
 * Two rules from CLI.md section 7 are structural here, not cosmetic:
 *
 *   - Never auto-run calibration. `cal zero i` with a load connected permanently
 *     poisons the offset and the firmware cannot detect it, so every calibration
 *     action goes through a confirmation that states the physical precondition.
 *   - Most output is prose. So a command's result is shown verbatim rather than
 *     parsed -- including the refusal text, which always names a physical cause.
 */
@Composable
fun ConfigureScreen(vm: MonitorViewModel) {
    val busy by vm.busy.collectAsState()
    val link by vm.link.collectAsState()
    val t by vm.telemetry.collectAsState()
    var last by remember { mutableStateOf<String?>(null) }
    var calState by remember { mutableStateOf<String?>(null) }
    var confirm by remember { mutableStateOf<Confirmation?>(null) }

    val run: (String) -> Unit = { cmd ->
        vm.launchCommandWith(cmd) { r ->
            last = if (r == null) {
                "no reply -- timed out"
            } else {
                (if (r.ok) "" else "exit " + r.exit + "\n") +
                    r.text.ifBlank { "(no output)" }
            }
        }
        Unit
    }
    val readCal: () -> Unit = {
        vm.launchCommandWith("cal") { r -> calState = r?.text }
        Unit
    }
    val guarded: (Confirmation) -> Unit = { confirm = it }

    // Which points are already set is the first thing to know before touching any of
    // this, and it costs a sub-100 ms command. Never cached across connections.
    LaunchedEffect(link) {
        if (link == Link.Ready && calState == null) readCal()
    }

    Column(
        Modifier.verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (link != Link.Ready) {
            Warn("Not connected. Nothing on this screen can be read or set until a board is.")
        }
        if (busy) Text("command in flight...", style = MaterialTheme.typography.labelMedium)

        last?.let { text ->
            Section("Last response") {
                Text(
                    text,
                    fontFamily = FontFamily.Monospace,
                    style = MaterialTheme.typography.bodySmall,
                )
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

            // What the board thinks right now, so the meter reading has something to be
            // compared against without leaving the screen.
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
                AssistChip(onClick = { run("cal save"); readCal() }, label = { Text("Save") })
                AssistChip(onClick = { readCal() }, label = { Text("Refresh state") })
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

            calState?.let { cs ->
                Spacer(Modifier.height(12.dp))
                Text("DEVICE CALIBRATION STATE", style = MaterialTheme.typography.labelSmall)
                Text(
                    cs,
                    fontFamily = FontFamily.Monospace,
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        // ------------------------------------------------------------ the rest

        Section("Read state") {
            // The prose overviews. Displayed, never parsed.
            Wrap {
                listOf(
                    "ver", "options", "soc", "curve", "cal", "shunt", "sensors",
                    "stream", "profile", "disp", "ble", "scan", "stats", "read", "env",
                ).forEach { c -> AssistChip(onClick = { run(c) }, label = { Text(c) }) }
            }
        }

        Section("Shunt and topology") {
            Text(
                "Get these right before calibrating: a wrong shunt value shows up as a " +
                    "gain outside ±10 %, which the device refuses rather than absorbs.",
                style = MaterialTheme.typography.bodySmall,
            )
            Spacer(Modifier.height(8.dp))
            MicroField("Shunt resistance", "mOhm", "100") {
                run("shunt " + Math.round(it * 1000.0))
            }
            Spacer(Modifier.height(8.dp))
            Choice("shunt loc", listOf("p", "n", "single", "auto")) { run("shunt loc " + it) }
            Choice("sense sign", listOf("normal", "invert")) { run("sense sign " + it) }
            Choice("sense vbuscomp", listOf("none", "add", "sub")) { run("sense vbuscomp " + it) }
            Choice("sense pgamax", listOf("1", "2", "4", "8")) { run("sense pgamax " + it) }
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
            RateRow("fast", "100", 20, 60_000, run)
            RateRow("calc", "500", 20, 60_000, run)
            RateRow("diag", "1000", 20, 60_000, run)
            RateRow("env", "10000", 20, 600_000, run)
            Spacer(Modifier.height(8.dp))
            Wrap {
                AssistChip(onClick = { run("stream csv") }, label = { Text("stream csv") })
                AssistChip(onClick = { run("stream on") }, label = { Text("stream on") })
                AssistChip(onClick = { run("stream off") }, label = { Text("stream off") })
            }
            Spacer(Modifier.height(8.dp))
            Choice("profile", listOf("continuous", "fast", "triggered")) {
                run("profile " + it)
            }
        }

        Section("Fuel gauge") {
            Text(
                "The pack endpoints and gauge behaviour. All of it persists; a rejected " +
                    "value prints the constraint that failed (v0 < v100 <= vfull).",
                style = MaterialTheme.typography.bodySmall,
            )
            Spacer(Modifier.height(8.dp))
            MicroField("Design capacity", "Ah", "44") { run("soc cap " + Micro.ampHours(it)) }
            MicroField("0 % resting OCV", "V", "11.80") { run("soc v0 " + Micro.volts(it)) }
            MicroField("100 % resting OCV", "V", "12.75") { run("soc v100 " + Micro.volts(it)) }
            MicroField("Absorption (full) voltage", "V", "14.40") {
                run("soc vfull " + Micro.volts(it))
            }
            MicroField("Internal resistance", "mOhm", "8") {
                run("soc rint " + Math.round(it * 1000.0))
            }
            MicroField("Taper current", "A", "2.2") { run("soc taper " + Micro.amps(it)) }
            MicroField("Rated discharge current", "A", "2.2") {
                run("soc irated " + Micro.amps(it))
            }
            PlainField("Rest before OCV is trusted", "s", "600") { run("soc rest " + it) }
            PlainField("Peukert k (Q8; 256 disables)", "q8", "300") { run("soc peukert " + it) }
            PlainField("Learning depth", "per mille", "500") { run("soc depth " + it) }
            MicroField("Force SoC", "%", "80") { run("soc set " + Micro.permille(it)) }
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
            Wrap {
                AssistChip(onClick = { run("disp on") }, label = { Text("on") })
                AssistChip(onClick = { run("disp off") }, label = { Text("off") })
                AssistChip(onClick = { run("disp screen auto") }, label = { Text("screen auto") })
                AssistChip(onClick = { run("disp screen 0") }, label = { Text("screen 0") })
            }
            Spacer(Modifier.height(8.dp))
            PlainField("Contrast (1-255; 0x40 default)", "", "64") { run("disp contrast " + it) }
        }

        Section("BLE and pairing") {
            Text(
                "open mode has no pairing at all: anything in range can run every " +
                    "command, calibration included. It is the default and it is a bench " +
                    "setting.",
                style = MaterialTheme.typography.bodySmall,
            )
            Spacer(Modifier.height(8.dp))
            Choice("ble pair", listOf("open", "bonded")) { mode ->
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
            PlainField("Fixed passkey (blank for random)", "", "random") {
                run("ble passkey " + it.ifBlank { "random" })
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

        Spacer(Modifier.height(24.dp))
    }

    confirm?.let { c ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            title = { Text(c.title) },
            text = { Text(c.body) },
            confirmButton = {
                Button(onClick = {
                    confirm = null
                    run(c.command)
                    // Whatever it did, the stored state changed; re-read rather than
                    // assume the command's own summary is the whole picture.
                    readCal()
                }) { Text("Run") }
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

@Composable
private fun Choice(label: String, options: List<String>, onPick: (String) -> Unit) {
    Column(Modifier.fillMaxWidth()) {
        Text(label, style = MaterialTheme.typography.labelMedium)
        Wrap {
            options.forEach { o ->
                // No selected state: the device is the authority on what is set, and this
                // app deliberately does not cache configuration across connections.
                FilterChip(selected = false, onClick = { onPick(o) }, label = { Text(o) })
            }
        }
    }
}

@Composable
private fun RateRow(group: String, hint: String, min: Int, max: Int, run: (String) -> Unit) {
    var text by remember { mutableStateOf("") }
    val v = text.toIntOrNull()
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        OutlinedTextField(
            value = text,
            onValueChange = { s -> text = s.filter { it.isDigit() } },
            label = { Text("stream " + group + " (ms)") },
            placeholder = { Text(hint) },
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            modifier = Modifier.weight(1f),
        )
        Spacer(Modifier.width(8.dp))
        Button(
            onClick = { if (v != null && v in min..max) run("stream " + group + " " + v) },
            enabled = v != null && v in min..max,
        ) { Text("Set") }
        TextButton(onClick = { run("stream " + group + " off") }) { Text("Off") }
    }
}

/**
 * A field in human units; the lambda turns the value into a micro-unit command.
 *
 * [prefill] offers the device's own live reading as a starting point. It is a starting
 * point and not a default: typing over it is the whole exercise, and submitting it
 * unchanged would just re-solve unity.
 */
@Composable
private fun MicroField(
    label: String,
    unit: String,
    hint: String,
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
                placeholder = { Text(hint) },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                modifier = Modifier.weight(1f),
            )
            Spacer(Modifier.width(8.dp))
            Button(onClick = { v?.let(onSet) }, enabled = v != null) { Text("Set") }
        }
        if (prefill != null && text.isEmpty()) {
            TextButton(onClick = { text = prefill }) { Text("use device reading " + prefill) }
        }
    }
}

/** A field whose value is already in the units the command wants. */
@Composable
private fun PlainField(label: String, unit: String, hint: String, onSet: (String) -> Unit) {
    var text by remember { mutableStateOf("") }
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        OutlinedTextField(
            value = text,
            onValueChange = { text = it },
            label = { Text(if (unit.isBlank()) label else label + " (" + unit + ")") },
            placeholder = { Text(hint) },
            singleLine = true,
            modifier = Modifier.weight(1f),
        )
        Spacer(Modifier.width(8.dp))
        Button(onClick = { onSet(text) }) { Text("Set") }
    }
}
