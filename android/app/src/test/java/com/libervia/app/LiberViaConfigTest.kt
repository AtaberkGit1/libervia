package com.libervia.app

import org.junit.Assert.assertEquals
import org.junit.Test

class LiberViaConfigTest {
    @Test
    fun universalDefaults_matchHardcodedSpecification() {
        assertEquals("-s 1 -d 1 -t 3", LiberViaVpnService.DEFAULT_DPI_FLAGS)
        assertEquals("1.1.1.1", LiberViaVpnService.PRIMARY_DNS)
        assertEquals("9.9.9.9", LiberViaVpnService.SECONDARY_DNS)
        assertEquals(1420, LiberViaVpnService.TUN_MTU)
    }
}
