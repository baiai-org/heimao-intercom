package net.openradiolink.app

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class MicrophoneGainTest {
    @Test fun unityAndSilenceRemainExact() {
        val original=shortArrayOf(-32768,-1000,0,1000,32767)
        val samples=original.copyOf();val level=MicrophoneGain(0).process(samples,samples.size,"mic")
        assertArrayEquals(original,samples);assertEquals(0L,level.clipped);assertEquals(32768,level.rawPeak)
        val silence=ShortArray(320);val quiet=MicrophoneGain(12).process(silence,320,"mic")
        assertArrayEquals(ShortArray(320),silence);assertEquals(-96.0,quiet.outputRmsDb,0.0)
    }
    @Test fun calibratedFixedGainAndSaturationNeverWrap() {
        val samples=shortArrayOf(1000,-1000,20000,-20000)
        val gain=MicrophoneGain(6);val level=gain.process(samples,4,"mic")
        assertArrayEquals(shortArrayOf(1995,-1995,32767,-32768),samples)
        assertEquals(2L,level.clipped);assertEquals(4L,level.samples)
        assertTrue(level.label.contains("过载"))
        val next=gain.process(shortArrayOf(100),1,"mic")
        assertEquals(2L,next.clipped);assertEquals(5L,next.samples)
        assertEquals(0L,MicrophoneGain(6).process(shortArrayOf(100),1,"mic").clipped)
    }
    @Test fun websocketAndHttpReceiveIdenticalOnceProcessedPcmWithPartialRead() {
        val samples=shortArrayOf(1000,-1000,12345)
        MicrophoneGain(6).process(samples,2,"mic")
        assertEquals(12345,samples[2].toInt())
        val http=microphonePacket(samples,2)
        val ws=microphonePacket(samples,2,7,9)
        assertArrayEquals(http,ws.copyOfRange(8,ws.size))
        val buffer=ByteBuffer.wrap(ws).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(7,buffer.int);assertEquals(9,buffer.int);assertEquals(1995,buffer.short.toInt())
        assertEquals(12,ws.size)
    }
    @Test fun unsupportedGainCannotBecomeAccidentalExtremeAmplification() {
        listOf(-3,1,13,60).forEach{assertThrows(IllegalArgumentException::class.java){MicrophoneGain(it)}}
    }
    @Test fun boardOverloadCannotLeakFromAnotherTransmissionOrDeviceSession() {
        val stats=org.json.JSONObject().put("sdr",org.json.JSONObject().put("sid",7).put("txid",9).put("clip",5))
        assertTrue(SessionView(status=stats,audioSid=7,audioTx=9).boardTxClipped())
        assertFalse(SessionView(status=stats,audioSid=7,audioTx=10).boardTxClipped())
        assertFalse(SessionView(status=stats,audioSid=8,audioTx=9).boardTxClipped())
        assertFalse(SessionView(status=stats).boardTxClipped())
    }
}
