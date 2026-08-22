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
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
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
import ru.vitska.powermon.ble.Micro
import ru.vitska.powermon.model.MonitorViewModel

/**
 * Everything the console can set, grouped the way CLI.md groups it.
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
    var last by remember { mutableStateOf<String?>(null) }
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
    val guarded: (Confirmation) -> Unit = { confirm = it }

    Column(
        Modifier.verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
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

        Section("Read state") {
            // The prose overviews. Displayed, never parsed.
            Wrap {
                listOf(
                    "ver", "options", "soc", "curve", "cal", "shunt", "sensors",
                    "stream", "profile", "disp", "ble", "scan", "stats", "read", "env",
                ).forEach { c -> AssistChip(onClick = { run(c) }, label = { Text(c) }) }
            }
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

        Section("Shunt and topology") {
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

        Section("Calibration") {
            Text(
                "Each of these takes a meter reading and solves for the trim itself. The " +
                    "preconditions are physical, and the firmware cannot check them for " +
                    "you -- read each confirmation.",
                style = MaterialTheme.typography.bodySmall,
            )
            Spacer(Modifier.height(8.dp))
            MicroField("Measured current now", "A", "1.959") { v ->
                guarded(
                    Confirmation(
                        "Set the current gain from " + v + " A?",
                        "The meter and the device must be measuring the same current, in " +
                            "the same direction, and it must exceed 10 mA. Takes about 17 s.",
                        "cal top i " + Micro.amps(v),
                    )
                )
            }
            MicroField("Measured voltage at rest", "V", "12.44") { v ->
                guarded(
                    Confirmation(
                        "Set the voltage gain from " + v + " V?",
                        "Take this reading AT REST. Under load the harness drop makes the " +
                            "solved gain wrong -- use the harness field below for that.",
                        "cal top v " + Micro.volts(v),
                    )
                )
            }
            MicroField("Terminal voltage under load", "V", "12.10") { v ->
                guarded(
                    Confirmation(
                        "Solve harness resistance from " + v + " V?",
                        "Needs at least 0.5 A flowing, and the reading must be taken at " +
                            "the battery terminals rather than at the board.",
                        "cal vpath " + Micro.volts(v),
                    )
                )
            }
            Spacer(Modifier.height(8.dp))
            Wrap {
                OutlinedButton(onClick = {
                    guarded(
                        Confirmation(
                            "Zero the current channel?",
                            "THE LOAD MUST BE DISCONNECTED. Running this with current " +
                                "flowing poisons the offset permanently and the firmware " +
                                "cannot detect it. Takes about 35 s.",
                            "cal zero i",
                        )
                    )
                }) { Text("cal zero i") }
                OutlinedButton(onClick = {
                    guarded(
                        Confirmation(
                            "Zero the voltage channel?",
                            "VBUS must be tied to GROUND, not merely disconnected -- a " +
                                "floating input reads a real voltage and the device will " +
                                "refuse. Takes about 68 s.",
                            "cal zero v",
                        )
                    )
                }) { Text("cal zero v") }
                OutlinedButton(onClick = { run("cal save") }) { Text("cal save") }
                OutlinedButton(onClick = {
                    guarded(
                        Confirmation(
                            "Erase the stored calibration?",
                            "Both live and stored trims are cleared. The board reverts to " +
                                "nominal scaling until it is calibrated again.",
                            "cal reset",
                        )
                    )
                }) { Text("cal reset") }
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
                Button(onClick = { confirm = null; run(c.command) }) { Text("Run") }
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

/** A field in human units; the lambda turns the value into a micro-unit command. */
@Composable
private fun MicroField(label: String, unit: String, hint: String, onSet: (Double) -> Unit) {
    var text by remember { mutableStateOf("") }
    val v = text.replace(',', '.').toDoubleOrNull()
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
