package net.openradiolink.app

import java.nio.ByteBuffer
import java.nio.ByteOrder

fun microphonePacket(samples: ShortArray,count: Int,sid: Long?=null,tx: Long?=null): ByteArray {
    require(count in 1..1020 && count<=samples.size)
    require((sid==null)==(tx==null))
    if(sid!=null)require(sid in 1..4294967295L && tx!! in 1..4294967295L)
    val packet=ByteBuffer.allocate(count*2+if(sid!=null)8 else 0).order(ByteOrder.LITTLE_ENDIAN)
    if(sid!=null){packet.putInt(sid.toInt());packet.putInt(tx!!.toInt())}
    for(i in 0 until count)packet.putShort(samples[i])
    return packet.array()
}
