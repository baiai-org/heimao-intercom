@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class,androidx.compose.foundation.layout.ExperimentalLayoutApi::class)
package net.openradiolink.app

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlin.math.abs
import kotlin.math.roundToInt

/** Six bands, named exactly as the firmware's &#96;eq_&lt;dir&gt;_&lt;band&gt;&#96; keys. */
internal object EqPresets {
    data class Band(val key:String,val freq:String,val name:String)
    data class Preset(val id:String,val label:String,val values:List<Int>)

    val bands=listOf(
        Band("bass","150Hz","低音"),
        Band("warm","400Hz","暖度"),
        Band("body","900Hz","人声"),
        Band("clear","2.0k","清晰"),
        Band("pres","3.2k","临场"),
        Band("air","3.8k","空气"),
    )
    /** Identical to the web panel's four curves; +6 dB boost ceiling is the firmware's. */
    val presets=listOf(
        Preset("voice","人声清晰",listOf(-4,-4,0,3,2,2)),
        Preset("punch","穿透增强",listOf(-8,-8,0,5,5,3)),
        Preset("warm","温暖厚重",listOf(2,3,2,0,-3,-4)),
        Preset("flat","当前平直",listOf(0,0,0,0,0,0)),
    )
    /** 发射默认曲线：-12 是固件下限；接收仍从全 0 开始。 */
    val micDefault=listOf(0,0,0,0,0,0)
    const val MIN=-12
    const val MAX=6
    fun clamp(value:Int)=value.coerceIn(MIN,MAX)
    fun active(values:List<Int>):Preset?=presets.firstOrNull{it.values==values}
    fun label(value:Int)=if(value>0)"+$value" else "$value"
    fun valid(values:List<Int>)=values.size==bands.size&&values.all{it in MIN..MAX}
}

@Composable
internal fun Equalizer(manager:DeviceManager,p:DeviceProfile,s:SessionView) {
    var open by remember(p.id){mutableStateOf(false)}
    val spk=(0..5).joinToString("/"){s.status.value("eq_spk.$it").ifBlank{"—"}}
    val mic=(0..5).joinToString("/"){s.status.value("eq_mic.$it").ifBlank{"—"}}
    SettingRow("6 段均衡器","收 $spk · 发 $mic",enabled=s.connected&&!s.maintenance){manager.pttUp(p.id);open=true}
    if(open) {
        ModalBottomSheet(sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true),onDismissRequest={open=false}) {
            EqualizerSheet(manager,p,s)
        }
    }
}

@Composable
private fun EqualizerSheet(manager:DeviceManager,p:DeviceProfile,s:SessionView) {
    var direction by remember{mutableStateOf("spk")}
    val spk=remember{initialValues(s,"spk")}
    val mic=remember{initialValues(s,"mic")}
    var saving by remember{mutableStateOf(false)}
    var error by remember{mutableStateOf("")}
    val values=if(direction=="spk")spk else mic
    val enabled=s.connected&&!s.maintenance&&!saving

    fun run(done:String,block:suspend BoardSession.()->Unit) {
        if(saving)return
        saving=true;error=""
        manager.action(p.id){
            try {
                block()
                withContext(Dispatchers.Main){manager.message(done)}
            } catch(e:CancellationException){throw e}
            catch(e:Exception){withContext(Dispatchers.Main){error=e.message ?: "保存失败，请重试"};throw e}
            finally{withContext(Dispatchers.Main){saving=false}}
        }
    }

    Column(Modifier.fillMaxWidth()
        .padding(bottom=WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding())
        .verticalScroll(rememberScrollState()).padding(horizontal=20.dp).padding(bottom=28.dp),
        verticalArrangement=Arrangement.spacedBy(12.dp)) {
        Row(Modifier.fillMaxWidth(),verticalAlignment=Alignment.CenterVertically) {
            Text("均衡器",style=MaterialTheme.typography.headlineSmall,modifier=Modifier.weight(1f))
            Text(if(saving)"正在写入…" else "固定 EQ · −12 ～ +6 dB",color=secondary,style=MaterialTheme.typography.bodySmall)
        }
        Row(horizontalArrangement=Arrangement.spacedBy(8.dp)) {
            listOf("spk" to "接收 EQ（SPK）","mic" to "发话 EQ（MIC）").forEach{(key,label)->
                FilledTonalButton(onClick={direction=key},enabled=!saving,
                    colors=ButtonDefaults.filledTonalButtonColors(
                        containerColor=if(direction==key)MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant),
                    shape=RoundedCornerShape(14.dp),modifier=Modifier.weight(1f)){Text(label,style=MaterialTheme.typography.labelLarge)}
            }
        }
        EqCurve(values,Modifier.fillMaxWidth().height(110.dp))
        Row(Modifier.fillMaxWidth(),horizontalArrangement=Arrangement.spacedBy(2.dp)) {
            EqPresets.bands.forEachIndexed{i,band->
                Column(Modifier.weight(1f),horizontalAlignment=Alignment.CenterHorizontally) {
                    Text(EqPresets.label(values[i]),style=MaterialTheme.typography.labelMedium,
                        color=if(values[i]==0)secondary else MaterialTheme.colorScheme.primary)
                    VerticalFader(values[i],enabled){values[i]=it}
                    Text(band.freq,style=MaterialTheme.typography.labelSmall,color=secondary,maxLines=1)
                    Text(band.name,style=MaterialTheme.typography.labelSmall,color=secondary,maxLines=1)
                }
            }
        }
        Text("拖动推子后点“保存这 6 段”；预设与“全部归 0”会立即写入手台。",color=secondary,style=MaterialTheme.typography.bodySmall)
        FlowRow(horizontalArrangement=Arrangement.spacedBy(8.dp),verticalArrangement=Arrangement.spacedBy(8.dp)) {
            EqPresets.presets.forEach{preset->
                val on=EqPresets.active(values)==preset
                OutlinedButton(onClick={
                    for(i in 0..5)values[i]=preset.values[i]
                    run("已应用：${preset.label}"){setEq(direction,preset.values)}
                },enabled=enabled,shape=RoundedCornerShape(14.dp),
                    colors=if(on)ButtonDefaults.outlinedButtonColors(containerColor=MaterialTheme.colorScheme.primaryContainer) else ButtonDefaults.outlinedButtonColors()){
                    Text(preset.label)
                }
            }
        }
        if(error.isNotBlank())Text(error,color=red,style=MaterialTheme.typography.bodySmall)
        Row(Modifier.fillMaxWidth(),horizontalArrangement=Arrangement.spacedBy(10.dp)) {
            OutlinedButton(onClick={
                run("已全部归 0 dB"){
                    resetEq()
                    withContext(Dispatchers.Main){for(i in 0..5){spk[i]=0;mic[i]=0}}
                }
            },enabled=enabled,shape=RoundedCornerShape(17.dp),modifier=Modifier.weight(1f)){Text("全部归 0 dB")}
            Button(onClick={run("均衡器已保存"){setEq(direction,values.toList())}},enabled=enabled,
                shape=RoundedCornerShape(17.dp),modifier=Modifier.weight(1f)){Text(if(saving)"正在保存…" else "保存这 6 段")}
        }
    }
}

private fun initialValues(s:SessionView,direction:String)=
    (0..5).map{s.status.value("eq_${direction}.$it").toIntOrNull()?.let(EqPresets::clamp) ?: 0}.toMutableStateList()

@Composable
private fun EqCurve(values:List<Int>,modifier:Modifier) {
    val line=MaterialTheme.colorScheme.primary
    val grid=MaterialTheme.colorScheme.outlineVariant
    Canvas(modifier.background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha=.16f),RoundedCornerShape(14.dp))) {
        val w=size.width;val h=size.height
        fun yOf(value:Int)=h*(EqPresets.MAX-value).toFloat()/(EqPresets.MAX-EqPresets.MIN)
        listOf(EqPresets.MAX,0,-6,EqPresets.MIN).forEach{mark->
            val y=yOf(mark)
            drawLine(grid,Offset(0f,y),Offset(w,y),strokeWidth=if(mark==0)2f else 1f)
        }
        val points=values.mapIndexed{i,v->Offset(w*i/5f,yOf(v))}
        val base=yOf(0)
        val fill=Path().apply {
            moveTo(points.first().x,base)
            points.forEach{lineTo(it.x,it.y)}
            lineTo(points.last().x,base)
            close()
        }
        drawPath(fill,Brush.verticalGradient(listOf(line.copy(alpha=.32f),line.copy(alpha=.02f)),startY=0f,endY=h))
        for(i in 0 until points.size-1)drawLine(line,points[i],points[i+1],strokeWidth=4f,cap=StrokeCap.Round)
        points.forEach{drawCircle(line,5f,it)}
    }
}

@Composable
private fun VerticalFader(value:Int,enabled:Boolean,changed:(Int)->Unit) {
    val line=MaterialTheme.colorScheme.primary
    val track=MaterialTheme.colorScheme.surfaceVariant
    var height by remember{mutableFloatStateOf(0f)}
    fun valueAt(y:Float):Int {
        if(height<=0f)return value
        val ratio=(y/height).coerceIn(0f,1f)
        return EqPresets.clamp((EqPresets.MAX-ratio*(EqPresets.MAX-EqPresets.MIN)).roundToInt())
    }
    Box(Modifier.height(148.dp).width(38.dp)
        .onSizeChanged{height=it.height.toFloat()}
        .pointerInput(enabled,height){
            if(!enabled)return@pointerInput
            detectTapGestures{offset->changed(valueAt(offset.y))}
        }
        .pointerInput(enabled,height){
            if(!enabled)return@pointerInput
            detectDragGestures(
                onDragStart={offset->changed(valueAt(offset.y))},
                onDrag={change,_->changed(valueAt(change.position.y))}
            )
        }) {
        Canvas(Modifier.fillMaxSize()) {
            val cx=size.width/2
            drawRoundRect(track,Offset(cx-3f,0f),Size(6f,size.height),CornerRadius(3f))
            val ty=size.height*(EqPresets.MAX-value).toFloat()/(EqPresets.MAX-EqPresets.MIN)
            drawRoundRect(line.copy(alpha=.22f),Offset(cx-3f,minOf(ty,size.height/2)),Size(6f,abs(ty-size.height/2)),CornerRadius(3f))
            drawCircle(if(enabled)line else track,9f,Offset(cx,ty))
        }
    }
}
