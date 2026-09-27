package ru.vitska.powermon.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
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
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.unit.dp
import ru.vitska.powermon.model.SocHistory

/**
 * The monitor's own SoC history (`hist`): a point every 10 minutes for 48 hours. The
 * right edge is now and each point sits where its age puts it, so a span shows exactly
 * that many hours. A gap in the data -- a reboot, an unseeded gauge -- breaks the line
 * rather than being bridged by one, since nothing was measured there.
 */
@Composable
fun HistoryChart(h: SocHistory?) {
    var spanH by remember { mutableIntStateOf(48) }

    Section("SoC history") {
        when {
            h == null -> Text("not read yet", style = MaterialTheme.typography.bodySmall)
            !h.supported -> Text(
                "The monitor's firmware keeps no history (it needs 0.9.0 or later).",
                style = MaterialTheme.typography.bodySmall,
            )
            h.points.isEmpty() -> Text(
                "No history yet: the monitor records a point every 10 minutes.",
                style = MaterialTheme.typography.bodySmall,
            )
            else -> {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    listOf(12, 24, 48).forEach { s ->
                        FilterChip(
                            selected = spanH == s,
                            onClick = { spanH = s },
                            label = { Text("$s h") },
                        )
                    }
                }
                Spacer(Modifier.height(4.dp))
                Chart(h, spanH)
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("-$spanH h", style = MaterialTheme.typography.labelSmall)
                    Text("-${spanH / 2} h", style = MaterialTheme.typography.labelSmall)
                    Text("now", style = MaterialTheme.typography.labelSmall)
                }
                if (h.states.isNotEmpty()) {
                    // What the band under the line means.
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

@Composable
private fun Chart(h: SocHistory, spanH: Int) {
    val line = MaterialTheme.colorScheme.primary
    val fill = line.copy(alpha = 0.18f)
    val grid = MaterialTheme.colorScheme.outlineVariant
    val ageNow = h.ageNowS()
    // Resolved here: a Canvas draw scope is not composable, so it cannot read the theme.
    val bandColor = listOf('C', 'A', 'F', 'D', 'E', 'S', 'R')
        .associateWith { c -> stateColor(stateName(c)!!) }

    Canvas(Modifier.fillMaxWidth().height(140.dp)) {
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

        // Runs of consecutive valid points become one path each; a gap starts a new run.
        val n = h.points.size
        var run = mutableListOf<Offset>()
        fun flush() {
            if (run.size >= 2) {
                val stroke = Path().apply {
                    moveTo(run.first().x, run.first().y)
                    run.drop(1).forEach { lineTo(it.x, it.y) }
                }
                val area = Path().apply {
                    addPath(stroke)
                    lineTo(run.last().x, ht)
                    lineTo(run.first().x, ht)
                    close()
                }
                drawPath(area, fill)
                drawPath(stroke, line, style = Stroke(width = 3f))
            } else if (run.size == 1) {
                drawCircle(line, 3f, run.first())
            }
            run = mutableListOf()
        }
        for (i in 0 until n) {
            val v = h.points[i]
            val age = ageNow + (n - 1 - i).toLong() * h.intervalS
            if (v == null || age > span) {
                flush()
                continue
            }
            val x = (w * (1.0 - age / span)).toFloat()
            val y = ht * (1f - v / 1000f)
            run.add(Offset(x, y))
        }
        flush()

        // The gauge's state along the bottom, as a timeline under the SoC: each point's
        // state holds for one interval after it, i.e. until the next point.
        val band = 6.dp.toPx()
        val step = (w * h.intervalS / span).toFloat()
        for (i in 0 until minOf(n, h.states.size)) {
            val c = bandColor[h.states[i]] ?: continue
            val age = ageNow + (n - 1 - i).toLong() * h.intervalS
            if (age > span) continue
            val x = (w * (1.0 - age / span)).toFloat()
            val x1 = minOf(w, x + step)
            if (x1 > x) drawRect(c, Offset(x, ht - band), androidx.compose.ui.geometry.Size(x1 - x, band))
        }
    }
}
