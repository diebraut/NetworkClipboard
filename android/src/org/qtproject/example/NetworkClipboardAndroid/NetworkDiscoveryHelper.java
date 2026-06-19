package org.qtproject.example.NetworkClipboardAndroid;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.Set;

public final class NetworkDiscoveryHelper
{
    private NetworkDiscoveryHelper()
    {
    }

    public static String candidatePrefixes(Context context)
    {
        Set<String> prefixes = new LinkedHashSet<>();

        try {
            for (NetworkInterface networkInterface :
                    Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!networkInterface.isUp() || networkInterface.isLoopback())
                    continue;
                for (InetAddress address :
                        Collections.list(networkInterface.getInetAddresses())) {
                    addPrefix(prefixes, address);
                }
            }
        } catch (Exception ignored) {
        }

        try {
            ConnectivityManager manager =
                (ConnectivityManager)context.getSystemService(Context.CONNECTIVITY_SERVICE);
            Network network = manager.getActiveNetwork();
            LinkProperties properties = manager.getLinkProperties(network);
            if (properties != null) {
                for (InetAddress dnsAddress : properties.getDnsServers())
                    addPrefix(prefixes, dnsAddress);
            }
        } catch (Exception ignored) {
        }

        return String.join(",", prefixes);
    }

    private static void addPrefix(Set<String> prefixes, InetAddress address)
    {
        if (!(address instanceof Inet4Address) || address.isLoopbackAddress())
            return;

        byte[] bytes = address.getAddress();
        prefixes.add(
            (bytes[0] & 0xff) + "."
            + (bytes[1] & 0xff) + "."
            + (bytes[2] & 0xff) + ".");
    }
}
