package net.openradiolink.app

import kotlinx.coroutines.*
import kotlinx.coroutines.test.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class ConnectionRulesTest {
    @Test fun publicPrefixAndIpv6RemainUserConfigured() {
        val base=EndpointAddress.parse("https://radio.example:8443/tunnel/board")
        assertEquals("https://radio.example:8443/tunnel/board/api/status",EndpointAddress.api(base,"api/status").toString())
        assertEquals("http://[2001:db8::1]:9000/",EndpointAddress.parse("[2001:db8::1]:9000").toString())
        assertEquals("http://[2001:db8::2]/",EndpointAddress.parse("2001:db8::2").toString())
        assertThrows(IllegalArgumentException::class.java){EndpointAddress.parse("https://user:secret@example.com/")}
        assertThrows(IllegalArgumentException::class.java){EndpointAddress.parse("https://example.com/?secret=1")}
    }
    @Test fun identityMergesEntrancesAndNeverMergesByIp() {
        val local=DeviceProfile(name="A",deviceId="hardware-A",endpoints=listOf(AccessPoint(address="http://192.168.1.8")),drafts=mapOf("draft" to "A"))
        val other=DeviceProfile(name="B",deviceId="hardware-B",endpoints=listOf(AccessPoint(address="http://192.168.1.8")),drafts=mapOf("draft" to "B"))
        assertEquals(2,mergeProfiles(listOf(local,other),other,"hardware-B").first.size)
        val remote=DeviceProfile(name="remote",endpoints=listOf(AccessPoint(address="https://example.com/proxy/",mode="PUBLIC")))
        val merged=mergeProfiles(listOf(local,other,remote),remote,"hardware-A")
        assertEquals(2,merged.first.size);assertEquals(local.id,merged.second.id)
        assertEquals(2,merged.second.endpoints.size);assertEquals(remote.endpoint().id,merged.second.selectedEndpoint)
        assertEquals("A",merged.second.drafts["draft"])
        assertEquals("B",merged.first.first{it.id==other.id}.drafts["draft"])
        assertThrows(IllegalArgumentException::class.java){mergeProfiles(listOf(local),local,"hardware-B")}
    }
    @Test fun dynamicProfilesAndCredentialsRoundTrip() {
        val profiles=(0 until 25).map { i ->DeviceProfile(name="Board $i",deviceId="device-$i",endpoints=listOf(
            AccessPoint(address="https://example.com/board/$i/",password="%20 + & = 测试",audioAddress="wss://example.com/audio/$i",mode="PUBLIC"))) }
        assertEquals(25,profiles.map{DeviceProfile.from(JSONObject(it.json().toString()))}.size)
        val decoded=DeviceProfile.from(JSONObject(profiles.first().json().toString()))
        assertEquals("%20 + & = 测试",decoded.endpoint().password)
        assertEquals(profiles.first().endpoint().address,decoded.endpoint().address)
    }
    @Test fun closingOldSessionPrecedesOpeningOnlyLatestSelection()=runTest {
        val events=mutableListOf<String>();val closed=CompletableDeferred<Unit>()
        class Fake(val name:String,val wait:Boolean=false):OwnedSession {
            override suspend fun connect(){events+="connect $name"}
            override suspend fun close(){events+="close $name";if(wait)closed.await();events+="closed $name"}
        }
        val slot=SingleDeviceSlot<Fake>();val a=Fake("A",true)
        slot.select{a}
        val b=launch{slot.select{events+="create B";Fake("B")}};runCurrent()
        assertSame(a,slot.current);assertFalse(events.contains("create B"))
        val c=launch{slot.select{events+="create C";Fake("C")}};runCurrent()
        closed.complete(Unit);advanceUntilIdle();b.join();c.join()
        assertFalse(events.contains("create B"));assertEquals("C",slot.current?.name)
        assertTrue(events.indexOf("closed A")<events.indexOf("create C"));assertFalse(slot.isCurrent(a))
        slot.select(null);assertNull(slot.current)
    }
    @Test fun failedConnectionClosesResources()=runTest {
        var closed=false
        val failed=object:OwnedSession {
            override suspend fun connect(){throw java.io.IOException("not reachable")}
            override suspend fun close(){closed=true}
        }
        val slot=SingleDeviceSlot<OwnedSession>()
        try{slot.select{failed};fail("expected failure")}catch(_:java.io.IOException){}
        assertTrue(closed);assertNull(slot.current)
    }
    @Test fun microphoneWireFramingUsesUnsignedIdsAndLittleEndianPcm() {
        val data=microphonePacket(shortArrayOf(0x1234,-1,Short.MIN_VALUE),3,4294967295L,2147483648L)
        assertArrayEquals(byteArrayOf(-1,-1,-1,-1,0,0,0,-128,0x34,0x12,-1,-1,0,-128),data)
        assertEquals(2048,microphonePacket(ShortArray(1020),1020,1,1).size)
        assertEquals(640,microphonePacket(ShortArray(320),320).size)
        assertThrows(IllegalArgumentException::class.java){microphonePacket(ShortArray(1021),1021,1,1)}
        assertThrows(IllegalArgumentException::class.java){microphonePacket(ShortArray(1),1,0,1)}
    }
}
