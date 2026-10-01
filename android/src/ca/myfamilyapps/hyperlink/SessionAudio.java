package ca.myfamilyapps.hyperlink;

import android.media.*;
import java.io.IOException;
import java.util.Arrays;
import java.util.concurrent.ArrayBlockingQueue;
import org.json.JSONObject;

/** Bounded playback of authorized host loopback audio. Never opens a microphone. */
final class SessionAudio implements Remote.AudioSink, AutoCloseable {
    private final ArrayBlockingQueue<byte[]> queue = new ArrayBlockingQueue<>(4);
    private final byte[] stream = new byte[16];
    private final AudioTrack output;
    private final Thread worker;
    private volatile boolean closed;
    SessionAudio(JSONObject format) throws Exception {
        if (format.getInt("sampleRate") != 48000 || format.getInt("channels") != 2 ||
            format.getInt("bits") != 16 || format.getInt("encoding") != 1) throw new IOException("Unsupported host audio format");
        String id = format.getString("id");
        if (!id.matches("[a-f0-9]{32}")) throw new IOException("Invalid audio stream identity");
        int[] order = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
        for (int i=0;i<16;i++) stream[i]=(byte)Integer.parseInt(id.substring(order[i]*2,order[i]*2+2),16);
        int minimum=AudioTrack.getMinBufferSize(48000,AudioFormat.CHANNEL_OUT_STEREO,AudioFormat.ENCODING_PCM_16BIT);
        if(minimum<=0) throw new IOException("Audio playback unavailable");
        output=new AudioTrack.Builder()
            .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
            .setAudioFormat(new AudioFormat.Builder().setSampleRate(48000).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
            .setBufferSizeInBytes(Math.max(minimum,8192)).setTransferMode(AudioTrack.MODE_STREAM).build();
        if(output.getState()!=AudioTrack.STATE_INITIALIZED){output.release();throw new IOException("Audio playback unavailable");}
        worker=new Thread(() -> {
            try {
                output.play();
                while(!closed){byte[] pcm=queue.take();int offset=0;
                    while(!closed && offset<pcm.length){int count=output.write(pcm,offset,pcm.length-offset,AudioTrack.WRITE_NON_BLOCKING);
                        if(count<0)throw new IOException("Audio device interrupted");
                        if(count==0)Thread.sleep(5);else offset+=count;
                    }
                }
            }catch(Exception ignored){}finally{closed=true;queue.clear();try{output.stop();}catch(IllegalStateException ignored){}output.release();}
        },"Hyperlink audio");worker.setDaemon(true);worker.start();
    }
    public void accept(byte[] packet){
        if(closed || packet.length<20 || packet.length>8208 || (packet.length-16)%4!=0)return;
        for(int i=0;i<16;i++)if(packet[i]!=stream[i])return;
        queue.offer(Arrays.copyOfRange(packet,16,packet.length));
    }
    public void close(){closed=true;queue.clear();worker.interrupt();}
}
