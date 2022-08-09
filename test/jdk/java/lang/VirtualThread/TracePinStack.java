/*
 *
 * Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
 * DO NOT ALTER OR REMOVE NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation. THL A29 Limited designates 
 * this particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

/**
 * @test
 * @library /test/lib
 * @run main/othervm TracePinStack
 * @summary Print stack trace of current virtual thread when it fail to yield.
 */

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.locks.LockSupport;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TracePinStack {
    public static class TestParkWithMonitor {
        public static void main(String[] args) throws Throwable {
            ExecutorService executor = Executors.newSingleThreadExecutor();

            Runnable target = new Runnable() {
                public void run() {
                    synchronized(this) {
                        System.out.println("before park");
                        LockSupport.parkNanos(this, 10000);
                        System.out.println("after park");
                    }
                }
            };

            Thread vt = Thread.ofVirtual().scheduler(executor).name("vt-0").unstarted(target);
            vt.start();
            vt.join();
            executor.shutdown();
        }
    }

    public static void main(String[] args) throws Throwable {
        ProcessBuilder pb = ProcessTools.createJavaProcessBuilder("-Djdk.tracePinnedThreads=full", "-XX:-YieldWithMonitor", TestParkWithMonitor.class.getName());
        OutputAnalyzer output = new OutputAnalyzer(pb.start());
        int exitValue = output.getExitValue();
        if (exitValue != 0) {
            //expecting a zero value
            throw new Error("Expected to get zero exit value");
        }

        System.out.println(output.getOutput());
        output.shouldContain("jdk.internal.misc.VirtualThreads.park");
        output.shouldContain("TracePinStack$TestParkWithMonitor$1.run");

        System.out.println("PASSED");
    }
}
