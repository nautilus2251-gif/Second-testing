package com.probe.g52;

final class Probe {
  static {
    System.loadLibrary("g52probe");
  }

  static native String run(String[] names, byte[][] blobs);

  private Probe() {}
}
