/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */
package console;

import java.util.concurrent.LinkedTransferQueue;
import javafx.application.Platform;

/**
 *
 * @author gmein
 */
public class SEMThread extends Thread {

    public enum Phase {
        WAITING_TO_CONNECT,
        WAITING_FOR_FRAME,
        WAITING_FOR_BYTES_OR_EFRAME,
        FINISHED,
        ABORTED
    }

    private Phase phase = Phase.WAITING_TO_CONNECT;
    private Phase lastPhase;
    private LinkedTransferQueue<SEMImage> ltq;
    private Runnable update;
    private Runnable restart;

    SEMThread(LinkedTransferQueue<SEMImage> q, Runnable update, Runnable restart) {
        this.ltq = q;
        this.update = update;
        this.restart = restart;
    }

    SEMPort semport = null;
    
    public void run() {
        try {
            System.out.println("Thread: starting.");
            
            Console.println("SEM Port: trying to initialize ...");
            this.phase = Phase.WAITING_TO_CONNECT;
            semport = new SEMPort();
            semport.initialize();
            Console.println("SEM Port initialized.");

            // main loop waiting for FRAME or BYTES or EFRAME messages
            this.phase = Phase.WAITING_FOR_FRAME;
            while (!this.isInterrupted() && this.phase != Phase.FINISHED && this.phase != Phase.ABORTED) {
                this.lastPhase = this.phase;
                this.phase = semport.processMessage(this.ltq, this.update, this.phase);
                Thread.yield();
            }
        } catch (InterruptedException ie) {
        } catch (Exception e) {
            System.out.println(e.toString());
        } finally {
            if (semport != null) {
                semport.shutdown();
            }
        }
        if (this.phase == Phase.ABORTED) {
            System.out.println("Aborted phase: " + this.lastPhase);
            Platform.runLater(this.restart);
        }
    }

}
