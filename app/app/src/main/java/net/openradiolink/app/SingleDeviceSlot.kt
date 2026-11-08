package net.openradiolink.app

import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.util.concurrent.atomic.AtomicLong

interface OwnedSession { suspend fun connect();suspend fun close() }

/** One owner, with old close completed before the next connection is created. */
class SingleDeviceSlot<T:OwnedSession> {
    private val lock=Mutex()
    private val selection=AtomicLong()
    @Volatile var current: T?=null;private set
    fun isCurrent(session: T)=current===session
    suspend fun select(factory: (()->T)?) {
        val ticket=selection.incrementAndGet()
        lock.withLock {
            if (ticket!=selection.get()) return
            withContext(NonCancellable) { current?.close();current=null }
            if (ticket!=selection.get() || factory==null) return
            val next=factory();current=next
            try { next.connect() }
            catch (e: Exception) {
                withContext(NonCancellable) { next.close();if(current===next)current=null }
                throw e
            }
            if (ticket!=selection.get()) withContext(NonCancellable) { next.close();if(current===next)current=null }
        }
    }
}
