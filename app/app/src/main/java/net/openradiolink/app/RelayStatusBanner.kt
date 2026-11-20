package net.openradiolink.app

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

@Composable
internal fun RelayStatusBanner(status: NetworkModes.RelayStatus) {
    val dark = LocalRadioDark.current
    val (fill, ink) = when(status.tone) {
        NetworkModes.RelayTone.CONNECTED -> if(dark)Color(0xFF0B2927) to Color(0xFF6EE7B7)
            else Color(0xFFDDF7EA) to Color(0xFF12583E)
        NetworkModes.RelayTone.WAITING -> if(dark)Color(0xFF312407) to Color(0xFFFDE68A)
            else Color(0xFFFFF3CB) to Color(0xFF754A00)
        NetworkModes.RelayTone.DOWN -> if(dark)Color(0xFF321724) to Color(0xFFFECDD3)
            else Color(0xFFFCE4E8) to Color(0xFF8F2140)
        NetworkModes.RelayTone.NEUTRAL -> MaterialTheme.colorScheme.surfaceVariant to
            MaterialTheme.colorScheme.onSurfaceVariant
    }
    Surface(
        color = fill,
        shape = RoundedCornerShape(12.dp),
        border = BorderStroke(1.dp, ink.copy(alpha = .36f)),
        modifier = Modifier.fillMaxWidth().semantics { liveRegion = LiveRegionMode.Polite }
    ) {
        Column(Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) {
            Text(status.title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold, color = ink)
            if(status.reason.isNotEmpty()) {
                Text(status.reason, style = MaterialTheme.typography.bodySmall, color = ink)
            }
        }
    }
}
