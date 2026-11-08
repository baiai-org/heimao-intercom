package net.openradiolink.app

import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.FormatListBulleted
import androidx.compose.material.icons.outlined.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable internal fun PreviewHeader(choose:()->Unit,info:()->Unit) {
    Row(Modifier.fillMaxWidth().statusBarsPadding().heightIn(min=48.dp).padding(horizontal=20.dp),verticalAlignment=Alignment.CenterVertically) {
        Row(Modifier.weight(1f).heightIn(min=48.dp).clickable(onClick=choose).semantics{contentDescription="选择手台"},verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(10.dp)) {
            Image(painterResource(R.drawable.brand_portrait),null,Modifier.size(30.dp).clip(RoundedCornerShape(9.dp)),contentScale=ContentScale.Fit)
            Text(Brand.APP,color=secondary,fontSize=16.sp,letterSpacing=.25.sp)
        }
        Surface(onClick=info,shape=RoundedCornerShape(50),color=panel,modifier=Modifier.size(48.dp)) {
            Box(contentAlignment=Alignment.Center){Icon(Icons.Outlined.Info,"设备状态",tint=secondary,modifier=Modifier.size(18.dp))}
        }
    }
}

@Composable internal fun PreviewNavigation(selected:Int,change:(Int)->Unit) {
    val labels=listOf("通联","信道","设备","更多")
    val icons=listOf(Icons.Outlined.Sensors,Icons.AutoMirrored.Outlined.FormatListBulleted,Icons.Outlined.Dashboard,Icons.Outlined.Tune)
    Column(Modifier.fillMaxWidth().navigationBarsPadding().padding(horizontal=12.dp).padding(top=4.dp,bottom=8.dp)) {
        Surface(color=panel,shape=RoundedCornerShape(25.dp),border=BorderStroke(1.dp,MaterialTheme.colorScheme.outlineVariant),shadowElevation=3.dp) {
            Row(Modifier.fillMaxWidth().padding(5.dp)) {
                labels.forEachIndexed{i,label->
                    Surface(color=if(selected==i)MaterialTheme.colorScheme.primaryContainer else panel,shape=RoundedCornerShape(20.dp),modifier=Modifier.weight(1f)) {
                        Column(Modifier.selectable(selected==i,role=Role.Tab,onClick={change(i)}).heightIn(min=60.dp).padding(vertical=6.dp),horizontalAlignment=Alignment.CenterHorizontally,verticalArrangement=Arrangement.Center) {
                            Icon(icons[i],null,tint=if(selected==i)blue else secondary,modifier=Modifier.size(20.dp))
                            Spacer(Modifier.height(4.dp));Text(label,fontSize=12.sp,lineHeight=18.sp,color=if(selected==i)blue else secondary)
                        }
                    }
                }
            }
        }
    }
}
