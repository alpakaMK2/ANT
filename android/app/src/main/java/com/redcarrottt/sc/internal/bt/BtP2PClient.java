package com.redcarrottt.sc.internal.bt;

import android.annotation.SuppressLint;

import com.redcarrottt.sc.internal.OnConnectResult;
import com.redcarrottt.sc.internal.OnDisconnectResult;
import com.redcarrottt.sc.internal.OnDiscoverResult;
import com.redcarrottt.sc.internal.P2PClient;

class BtP2PClient extends P2PClient {
    @Override
    protected void discoverImpl(OnDiscoverResult onDiscoverResult) {
        // Assume that target device has already been bonded.
        onDiscoverResult.onDoneDiscover(true);
    }

    @Override
    protected void connectImpl(OnConnectResult onConnectResult) {
        // Assume that target device has already been bonded.
        onConnectResult.onDoneConnect(true);
    }

    @Override
    protected void disconnectImpl(OnDisconnectResult onDisconnectResult) {
        // Assume that target device has already been bonded.
        onDisconnectResult.onDoneDisconnect(true);
    }

    // Singleton
    public static BtP2PClient getSingleton() {
        if(sSingleton == null) {
            sSingleton = new BtP2PClient();
        }
        return sSingleton;
    }
    @SuppressLint("StaticFieldLeak")
    private static BtP2PClient sSingleton = null;

    // Constructor
    private BtP2PClient() {
    }

    // Attributes
}
