package ru.vitska.powermon.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.unit.dp
import ru.vitska.powermon.model.SocHistory

/**
 * The monitor's own SoC history (`hist`). The right edge is now and each point sits where
 * its age puts it, so a span shows exactly that many hours. A gap in the data -- a
 * reboot, an unseeded gauge -- breaks the line rather than being bridged by one, since
 * nothing was measured there.
 *
 * The spans on offer come from the device: its interval is settable and its ring is a
 * fixed number of points, so what it holds -- a half of that, and a quarter -- is the
 * honest menu. A fixed 12/24/48 would offer hours the monitor never recorded.
 */
@Composable
fun HistoryChart(h: SocHistory?) {
    var spanH by remember { mutableIntStateOf(0) }

    Section("SoC history") {
        when {
            h == null -> Text("not read yet", style = MaterialTheme.typography.bodySmall)
            !h.supported -> Text(
                "The monitor's firmware keeps no history (it needs 0.9.0 or later).",
                style = MaterialTheme.typography.bodySmall,
            )
            h.points.isEmpty() -> Text(
                "No history yet: the monitor records a point every " +
                    (h.intervalS / 60).coerceAtLeast(1) + " minutes.",
                style = MaterialTheme.typography.bodySmall,
            )
            else -> {
                val full = h.coverH.coerceAtLeast(1)
                val spans = listOf(full / 4, full / 2, full).filter { it > 0 }.distinct()
                // Not stored back into spanH: the monitor's interval can change under us,
                // and a remembered span it no longer covers would draw an empty left half.
                val span = if (spanH in spans) spanH else spans.last()
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    spans.forEach { s ->
                        FilterChip(
                            selected = span == s,
                            onClick = { spanH = s },
                            label = { Text("$s h") },
                        )
                    }
                }
                Spacer(Modifier.height(4.dp))
                val step = hourStep(span)
                Row(Modifier.fillMaxWidth()) {
                    // Percentages down the left, against the same quarters the grid is
                    // drawn at. SpaceBetween puts the first and last hard against the
                    // ends, which is where 100 and 0 belong.
                    Column(
                        Modifier.width(AXIS_W).height(CHART_H),
                        verticalArrangement = Arrangement.SpaceBetween,
                        horizontalAlignment = Alignment.End,
                    ) {
                        listOf(100, 75, 50, 25, 0).forEach {
                            Text("$it", style = MaterialTheme.typography.labelSmall)
                        }
                    }
                    Spacer(Modifier.width(4.dp))
                    Chart(h, span, step)
                }
                // Hour labels under the plot only, so they line up with the verticals.
                Row(Modifier.fillMaxWidth()) {
                    Spacer(Modifier.width(AXIS_W + 4.dp))
                    Row(Modifier.weight(1f), horizontalArrangement = Arrangement.SpaceBetween) {
                        val ticks = span / step
                        (ticks downTo 0).forEach { t ->
                            Text(
                                if (t == 0) "now" else "-${t * step}h",
                                style = MaterialTheme.typography.labelSmall,
                            )
                        }
                    }
                }
                if (h.states.isNotEmpty()) {
                    // What the line and fill colours mean.
                    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        listOf("CHARGE", "FULL", "DISCHARGE", "EMPTY", "REST").forEach { s ->
                            Text("■ " + s.lowercase(), color = stateColor(s),
                                style = MaterialTheme.typography.labelSmall)
                        }
                    }
                }
            }
        }
    }
}

/**
 * Hours between vertical grid lines: the smallest step that leaves at most four
 * divisions. The span comes from the monitor's recording interval and ring size, so the
 * grid follows how often the device records without this knowing that it does.
 */
private fun hourStep(spanH: Int): Int =
    listOf(1, 2, 3, 4, 6, 8, 12, 24, 48).firstOrNull { spanH <= it * 4 } ?: spanH

private val CHART_H = 140.dp
private val AXIS_W = 22.dp

@Composable
private fun Chart(h: SocHistory, spanH: Int, stepH: Int) {
    val line = MaterialTheme.colorScheme.primary
    val grid = MaterialTheme.colorScheme.outlineVariant
    val ageNow = h.ageNowS()
    // Resolved here: a Canvas draw scope is not composable, so it cannot read the theme.
    val stateLine = listOf('C', 'A', 'F', 'D', 'E', 'S', 'R')
        .associateWith { c -> stateColor(stateName(c)!!) }

    Canvas(Modifier.fillMaxWidth().height(CHART_H)) {
        val w = size.width
        val ht = size.height
        val span = spanH * 3600.0

        // Grid at 0, 25, 50, 75, 100 %.
        for (q in 0..4) {
            val y = ht * q / 4f
            drawLine(
                grid, Offset(0f, y), Offset(w, y), strokeWidth = 1f,
                pathEffect = if (q == 0 || q == 4) null else PathEffect.dashPathEffect(floatArrayOf(6f, 6f)),
            )
        }
        // And on whole hours back from now, so a feature can be placed in time rather
        // than only in shape. The right edge is now and carries no line of its own.
        var t = stepH
        while (t < spanH) {
            val x = w * (1f - t.toFloat() / spanH)
            drawLine(
                grid, Offset(x, 0f), Offset(x, ht), strokeWidth = 1f,
                pathEffect = PathEffect.dashPathEffect(floatArrayOf(6f, 6f)),
            )
            t += stepH
        }

        /*
         * Each segment -- from one point to the next -- takes the colour of the gauge's
         * state at its start, line and fill alike, so the curve itself says when the pack
         * was charging, full, discharging or resting. A point with no state (a gap, or
         * history from before states were recorded) draws in the plain accent colour.
         * A gap in the data starts a new run rather than being bridged.
         */
        val n = h.points.size
        var prev: Offset? = null
        var prevColor = line
        for (i in 0 until n) {
            val v = h.points[i]
            val age = ageNow + (n - 1 - i).toLong() * h.intervalS
            if (v == null || age > span) {
                prev?.let { drawCircle(prevColor, 3f, it) }
                prev = null
                continue
            }
            val p = Offset((w * (1.0 - age / span)).toFloat(), ht * (1f - v / 1000f))
            val c = stateLine[h.states.getOrNull(i) ?: '-'] ?: line
            prev?.let { a ->
                val area = Path().apply {
                    moveTo(a.x, a.y); lineTo(p.x, p.y); lineTo(p.x, ht); lineTo(a.x, ht); close()
                }
                drawPath(area, prevColor.copy(alpha = 0.22f))
                drawLine(prevColor, a, p, strokeWidth = 3f)
            }
            prev = p
            prevColor = c
        }
    }
}
