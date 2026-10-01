package ca.myfamilyapps.hyperlink;

import java.io.*;
import java.nio.file.Files;
import java.util.concurrent.*;
import java.awt.image.BufferedImage;
import javax.imageio.ImageIO;

public final class RecordingCheck {
    public static void main(String[] args) throws Exception {
        File folder=new File(args[0],"recordings");if(!folder.mkdirs())throw new AssertionError("Test directory");
        BufferedImage image=new BufferedImage(64,32,BufferedImage.TYPE_INT_RGB);
        java.awt.Graphics2D graphics=image.createGraphics();graphics.setColor(java.awt.Color.CYAN);graphics.fillRect(0,0,64,32);graphics.dispose();
        ByteArrayOutputStream encoded=new ByteArrayOutputStream();if(!ImageIO.write(image,"jpeg",encoded))throw new AssertionError("JPEG fixture");byte[] jpeg=encoded.toByteArray();
        MjpegRecording recorder=new MjpegRecording(folder,64,32);recorder.accept(jpeg);recorder.accept(jpeg);recorder.close();File complete=recorder.completion.get(10,TimeUnit.SECONDS);
        if(!complete.isFile() || folder.listFiles((dir,name)->name.endsWith(".part")).length!=0)throw new AssertionError("Finalization");
        MjpegRecording empty=new MjpegRecording(folder,64,32);empty.close();denied(empty.completion);
        MjpegRecording invalid=new MjpegRecording(folder,64,32);invalid.accept(new byte[]{0,0,0,0});invalid.close();denied(invalid.completion);
        if(folder.listFiles((dir,name)->name.endsWith(".part")).length!=0)throw new AssertionError("Failed recording cleanup");
        byte[] before=Files.readAllBytes(complete.toPath());File staged=new File(folder,"replacement.part");Files.write(staged.toPath(),new byte[]{1,2,3});
        boolean refused=false;try{MjpegRecording.publish(staged,complete);}catch(IOException ex){refused=true;}
        if(!refused || !java.util.Arrays.equals(before,Files.readAllBytes(complete.toPath())))throw new AssertionError("Existing file replaced");Files.delete(staged.toPath());
        try(OutputStream output=new FileOutputStream(new File(folder,"timing.mkv"))){MjpegRecording.header(output,64,32);for(long ms:new long[]{0,125,500})MjpegRecording.sample(output,jpeg,ms);}
        byte[] stream=new byte[16];stream[0]=7;byte[] packet=new byte[1936];System.arraycopy(stream,0,packet,0,16);
        for(int n=0;n<480;n++){short tone=(short)(Math.sin(n*2*Math.PI*1000/48000)*12000);for(int c=0;c<2;c++){packet[16+n*4+c*2]=(byte)tone;packet[17+n*4+c*2]=(byte)(tone>>8);}}
        MjpegRecording sound=new MjpegRecording(folder,64,32,stream);sound.accept(jpeg);sound.acceptAudio(packet);
        byte[] wrong=packet.clone();wrong[0]^=1;boolean rejected=false;try{sound.acceptAudio(wrong);}catch(IOException expected){rejected=true;}if(!rejected)throw new Exception("Wrong audio stream accepted");
        sound.close();File clip=sound.completion.get(5,TimeUnit.SECONDS);Files.move(clip.toPath(),new File(folder,"audio.mkv").toPath());
        System.out.println("Android recording finalization, audio stream validation, empty/invalid cleanup and no-overwrite checks passed.");
    }
    private static void denied(CompletableFuture<File> future) throws Exception {boolean refused=false;try{future.get(10,TimeUnit.SECONDS);}catch(ExecutionException ex){refused=true;}if(!refused)throw new AssertionError("Invalid recording published");}
}
