package net.openradiolink.app

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.MoreHoriz
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private data class EqSlotEdit(val op: EqSlotOp, val slot: Int, val base: EqSlots)
private data class EqSlotConfirm(val message: String, val action: () -> Unit)

@Composable
internal fun UserEqSlots(
    manager: DeviceManager, profile: DeviceProfile, state: SessionView, enabled: Boolean,
    curves: () -> EqCurves, onApplied: (EqCurves) -> Unit, onBusy: (Boolean) -> Unit
) {
    if (!state.eqPresetsSupported) return
    val device = state.deviceId
    val canReach = state.profileId == profile.id && state.connected && !state.maintenance && device.isNotEmpty()
    val scope = rememberCoroutineScope()
    var saved by remember(profile.id, device) { mutableStateOf<EqSlots?>(null) }
    var edit by remember(profile.id, device) { mutableStateOf<EqSlotEdit?>(null) }
    var confirmation by remember(profile.id, device) { mutableStateOf<EqSlotConfirm?>(null) }
    var draft by remember(profile.id, device) { mutableStateOf("") }
    var feedback by remember(profile.id, device) { mutableStateOf("") }
    var failed by remember(profile.id, device) { mutableStateOf(false) }
    var busy by remember(profile.id, device) { mutableStateOf(false) }
    var epoch by remember(profile.id, device) { mutableLongStateOf(0L) }

    LaunchedEffect(profile.id, device, canReach) {
        if (!canReach) return@LaunchedEffect
        while (isActive) {
            if (!busy) {
                val started = epoch
                manager.action(profile.id) {
                    try {
                        val current = readEqSlots()
                        withContext(Dispatchers.Main) {
                            if (started == epoch && current.device == device) {
                                if (saved == null) { feedback = ""; failed = false }
                                saved = current
                            }
                        }
                    } catch (error: CancellationException) {
                        throw error
                    } catch (error: Exception) {
                        withContext(Dispatchers.Main) {
                            if (started == epoch && saved == null) {
                                feedback = error.message ?: "读取 EQ 保存栏失败"
                                failed = true
                            }
                        }
                        if (error is CertificateFailure) throw error
                    }
                }?.join()
            }
            delay(3000)
        }
    }

    fun submit(write: EqSlotWrite) {
        if (!canReach || !enabled || busy || write.base.device != device) return
        ++epoch
        busy = true
        onBusy(true)
        feedback = "正在写入…"
        failed = false
        val job = manager.action(profile.id) {
            try {
                val result = writeEqSlot(write)
                withContext(Dispatchers.Main) {
                    saved = result
                    edit = null
                    if (write.op == EqSlotOp.APPLY) onApplied(result.active)
                    feedback = write.op.feedback
                    manager.message(feedback)
                }
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                withContext(Dispatchers.Main) {
                    if (error is EqSlotUnconfirmed && error.latest?.device == device) {
                        saved = error.latest
                        edit = edit?.copy(base = error.latest)
                    }
                    feedback = error.message ?: "写入未确认"
                    failed = true
                }
                if (error is CertificateFailure) throw error
            }
        }
        if (job == null) {
            busy = false
            onBusy(false)
            failed = true
            feedback = "手台当前不可操作，请重新连接"
        } else {
            job.invokeOnCompletion { scope.launch { busy = false; onBusy(false) } }
        }
    }

    fun startEdit(op: EqSlotOp, slot: Int, base: EqSlots) {
        edit = EqSlotEdit(op, slot, base)
        draft = base.slots[slot - 1]?.name.orEmpty()
        feedback = ""
        failed = false
    }

    val interactive = enabled && canReach && !busy
    val allowed = interactive && edit == null && confirmation == null
    Text("用户 EQ 预设", style = MaterialTheme.typography.titleMedium)
    repeat(EqSlots.COUNT) { index ->
        val base = saved
        val slot = index + 1
        val item = base?.slots?.get(index)
        val title = "槽位 $slot" + if (item != null) " · ${item.name}" else if (base != null) " · 空槽" else ""
        Surface(
            color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = .45f),
            shape = RoundedCornerShape(14.dp), modifier = Modifier.fillMaxWidth()
        ) {
            Row(Modifier.padding(horizontal = 10.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(title, modifier = Modifier.weight(1f), style = MaterialTheme.typography.titleSmall,
                    maxLines = 1, overflow = TextOverflow.Ellipsis)
                if (base != null && item != null) {
                    TextButton(onClick = { submit(EqSlotWrite(EqSlotOp.APPLY, slot, base)) }, enabled = allowed) {
                        Text("应用", maxLines = 1)
                    }
                    Box {
                        var expanded by remember(profile.id, device, allowed) { mutableStateOf(false) }
                        IconButton(onClick = { expanded = true }, enabled = allowed) {
                            Icon(Icons.Default.MoreHoriz, contentDescription = "槽位 $slot 更多操作")
                        }
                        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                            Text(item.name, modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
                                style = MaterialTheme.typography.bodySmall)
                            DropdownMenuItem(text = { Text("重命名") }, onClick = {
                                expanded = false
                                startEdit(EqSlotOp.RENAME, slot, base)
                            })
                            DropdownMenuItem(text = { Text("覆盖") }, onClick = {
                                expanded = false
                                confirmation = EqSlotConfirm("覆盖「${item.name}」的 EQ 曲线？") {
                                    startEdit(EqSlotOp.SAVE, slot, base)
                                }
                            })
                            DropdownMenuItem(text = { Text("删除") }, onClick = {
                                expanded = false
                                confirmation = EqSlotConfirm("删除「${item.name}」？") {
                                    submit(EqSlotWrite(EqSlotOp.DELETE, slot, base))
                                }
                            })
                        }
                    }
                } else {
                    TextButton(onClick = { if (base != null) startEdit(EqSlotOp.SAVE, slot, base) },
                        enabled = allowed && base != null) { Text("保存", maxLines = 1) }
                }
            }
        }
    }

    edit?.let { entry ->
        val savingCurve = entry.op == EqSlotOp.SAVE
        val label = if (savingCurve) "保存到槽位 ${entry.slot}" else "重命名槽位 ${entry.slot}"
        OutlinedTextField(
            draft, { draft = it }, label = { Text(label) }, singleLine = true,
            enabled = interactive, modifier = Modifier.fillMaxWidth()
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = {
                val name = draft.trim()
                val current = if (savingCurve) curves() else null
                when {
                    !EqUserSlot.nameValid(name) -> {
                        failed = true; feedback = "请输入名称，最多 24 字，不可包含控制字符"
                    }
                    current != null && (!current.editable() || !entry.base.active.editable()) -> {
                        failed = true; feedback = "现有曲线超出 −12 到 +6 dB，请先调整并保存当前 EQ"
                    }
                    else -> submit(EqSlotWrite(entry.op, entry.slot, entry.base, name, current))
                }
            }, enabled = interactive) { Text("保存") }
            TextButton(onClick = { edit = null; feedback = ""; failed = false }, enabled = !busy) { Text("取消") }
        }
    }
    val notice = when {
        !canReach -> "手台离线，等待重连"
        feedback.isNotBlank() -> feedback
        saved == null -> "正在读取 EQ 保存栏…"
        else -> ""
    }
    if (notice.isNotEmpty()) Text(notice, color = if (failed) red else secondary, style = MaterialTheme.typography.bodySmall)
    confirmation?.let { request ->
        AlertDialog(
            onDismissRequest = { confirmation = null },
            text = { Text(request.message) },
            confirmButton = {
                TextButton(onClick = { confirmation = null; request.action() }, enabled = interactive) { Text("确认") }
            },
            dismissButton = { TextButton(onClick = { confirmation = null }) { Text("取消") } }
        )
    }
}
