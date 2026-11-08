package net.openradiolink.app

import androidx.activity.compose.LocalActivity
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.clickable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.view.WindowCompat

internal val background:Color @Composable get()=MaterialTheme.colorScheme.background
internal val panel:Color @Composable get()=MaterialTheme.colorScheme.surface
internal val secondary:Color @Composable get()=MaterialTheme.colorScheme.onSurfaceVariant
internal val blue:Color @Composable get()=MaterialTheme.colorScheme.primary
internal val green:Color @Composable get()=if(LocalRadioDark.current)Color(0xFF6ED7A0) else Color(0xFF18744D)
internal val amber:Color @Composable get()=if(LocalRadioDark.current)Color(0xFFFFCF80) else Color(0xFF855000)
/* Mode accents, one per network mode: local green / P2P purple / relay amber, the
 * same trio the web panel paints its mode buttons with (#34d399 / #a78bfa / #fbbf24). */
internal val violet:Color @Composable get()=if(LocalRadioDark.current)Color(0xFFA78BFA) else Color(0xFF6D4AFF)
internal val red:Color @Composable get()=MaterialTheme.colorScheme.error
internal val LocalRadioDark=staticCompositionLocalOf{true}
internal val yesNo=listOf("0" to "关闭","1" to "开启")
internal val modes=listOf("0" to "本地电台","1" to "公网直连 P2P","2" to "网络中继 MQTT")

@Composable internal fun RadioTheme(appearance:String,content:@Composable ()->Unit) {
    val dark=when(appearance){"light"->false;"system"->isSystemInDarkTheme();else->true}
    // Match the selected HTML preview's palette; do not inherit Material's purple containers.
    val scheme=if(dark)darkColorScheme(primary=Color(0xFF69B0FF),onPrimary=Color(0xFF081725),
        primaryContainer=Color(0xFF21334C),onPrimaryContainer=Color(0xFF69B0FF),
        secondaryContainer=Color(0xFF21334C),onSecondaryContainer=Color(0xFF69B0FF),
        background=Color(0xFF11151C),surface=Color(0xFF1D222B),surfaceVariant=Color(0xFF343B47),
        surfaceContainerLowest=Color(0xFF11151C),surfaceContainerLow=Color(0xFF1D222B),surfaceContainer=Color(0xFF1D222B),surfaceContainerHigh=Color(0xFF272D38),surfaceContainerHighest=Color(0xFF343B47),
        onBackground=Color(0xFFF0F3F8),onSurface=Color(0xFFF0F3F8),onSurfaceVariant=Color(0xFFAFB9C8),
        outline=Color(0xFF778392),outlineVariant=Color(0xFF343B47),error=Color(0xFFFF8992))
    else lightColorScheme(primary=Color(0xFF0767CD),onPrimary=Color.White,
        primaryContainer=Color(0xFFE7F0FC),onPrimaryContainer=Color(0xFF0767CD),
        secondaryContainer=Color(0xFFE7F0FC),onSecondaryContainer=Color(0xFF0767CD),
        background=Color(0xFFF2F4F8),surface=Color.White,surfaceVariant=Color(0xFFDDE3EB),
        surfaceContainerLowest=Color.White,surfaceContainerLow=Color.White,surfaceContainer=Color(0xFFF2F4F8),surfaceContainerHigh=Color(0xFFE7ECF3),surfaceContainerHighest=Color(0xFFDDE3EB),
        onBackground=Color(0xFF17202E),onSurface=Color(0xFF17202E),onSurfaceVariant=Color(0xFF566173),
        outline=Color(0xFF778392),outlineVariant=Color(0xFFDDE3EB),error=Color(0xFFBD2635))
    val view=LocalView.current
    val activity=LocalActivity.current
    SideEffect { activity?.window?.let { window->WindowCompat.getInsetsController(window,view).apply {
        isAppearanceLightStatusBars=!dark;isAppearanceLightNavigationBars=!dark
    }
        // Android 7 cannot draw dark navigation icons. Keep its system bar dark in both themes.
        if(android.os.Build.VERSION.SDK_INT<26){
            @Suppress("DEPRECATION")
            window.navigationBarColor=0xFF131D29.toInt()
        }
    } }
    CompositionLocalProvider(LocalRadioDark provides dark) {
        MaterialTheme(colorScheme=scheme,shapes=Shapes(small=RoundedCornerShape(14.dp),medium=RoundedCornerShape(16.dp),large=RoundedCornerShape(20.dp),extraLarge=RoundedCornerShape(26.dp)),typography=Typography(
            headlineLarge=MaterialTheme.typography.headlineLarge.copy(fontWeight=FontWeight.SemiBold),
            titleLarge=MaterialTheme.typography.titleLarge.copy(fontWeight=FontWeight.SemiBold),
            bodyLarge=MaterialTheme.typography.bodyLarge.copy(fontSize=16.sp,lineHeight=24.sp),
            bodyMedium=MaterialTheme.typography.bodyMedium.copy(fontSize=14.sp,lineHeight=21.sp),
            bodySmall=MaterialTheme.typography.bodySmall.copy(fontSize=13.sp,lineHeight=19.sp)
        ),content=content)
    }
}

@Composable internal fun Section(title:String,dense:Boolean=false,content:@Composable ColumnScope.()->Unit) {
    Column(verticalArrangement=Arrangement.spacedBy(8.dp)) {
        Text(title,style=MaterialTheme.typography.titleSmall,color=secondary,modifier=Modifier.padding(start=6.dp))
        Surface(shape=RoundedCornerShape(20.dp),color=panel) {
            Column(Modifier.fillMaxWidth().padding(horizontal=16.dp,vertical=4.dp),verticalArrangement=Arrangement.spacedBy(if(dense)0.dp else 4.dp),content=content)
        }
    }
}

@Composable internal fun SettingRow(title:String,value:String="",subtitle:String="",enabled:Boolean=true,click:()->Unit) {
    val largeText=LocalDensity.current.fontScale>1.3f
    Row(Modifier.fillMaxWidth().clickable(enabled=enabled,onClick=click).heightIn(min=58.dp).padding(vertical=10.dp),
        verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(10.dp)) {
        Column(Modifier.weight(1f),verticalArrangement=Arrangement.spacedBy(2.dp)) {
            Text(title,style=MaterialTheme.typography.bodyLarge)
            if(subtitle.isNotBlank())Text(subtitle,style=MaterialTheme.typography.bodySmall,color=secondary)
            if(largeText && value.isNotBlank())Text(value,style=MaterialTheme.typography.bodyMedium,color=secondary)
        }
        if(!largeText && value.isNotBlank())Text(value,style=MaterialTheme.typography.bodyMedium,color=secondary,modifier=Modifier.weight(.9f),textAlign=androidx.compose.ui.text.style.TextAlign.End)
        Icon(Icons.AutoMirrored.Filled.KeyboardArrowRight,null,tint=secondary)
    }
}
