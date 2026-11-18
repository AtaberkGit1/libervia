# Keep JNI entrypoints and native callbacks in LiberViaVpnService
-keepclassmembers class com.libervia.app.LiberViaVpnService {
    public boolean protectSocket(int);
    private native int nativeStartEngine(int, java.lang.String, java.lang.String);
    private native void nativeStopEngine();
    public native long[] nativeGetStats();
}

-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}
