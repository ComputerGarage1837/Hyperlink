package ca.myfamilyapps.hyperlink;

import java.io.*;
import java.net.URI;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.concurrent.*;
import javax.net.ssl.SSLParameters;
import org.java_websocket.client.WebSocketClient;
import org.java_websocket.handshake.ServerHandshake;

/** Bounded byte transport. TLS with the Windows host runs inside this socket. */
final class RelayTransport implements Closeable {
    static final String ENDPOINT="wss://hyperlink.myfamilyapps.ca/relay/";
    private final ArrayBlockingQueue<Object> messages=new ArrayBlockingQueue<>(16);
    private volatile boolean closed;
    private volatile Exception failure;
    private byte[] pending;private int position;
    int readTimeout=15000;
    private final WebSocketClient socket;
    private final Object writeLock=new Object();
    private RelayTransport(String path)throws Exception {
        if(!path.equals("control")&&!path.equals("session"))throw new IOException("Invalid relay endpoint");
        socket=new WebSocketClient(new URI(ENDPOINT+path),new org.java_websocket.drafts.Draft_6455(java.util.Collections.<org.java_websocket.extensions.IExtension>emptyList(),65536)) {
            public void onOpen(ServerHandshake handshake){}
            public void onMessage(String message){if(message.length()>16384){close();return;}enqueue(message);}
            public void onMessage(ByteBuffer bytes){if(bytes.remaining()<1||bytes.remaining()>65536){close();return;}byte[] data=new byte[bytes.remaining()];bytes.get(data);enqueue(data);}
            public void onClose(int code,String reason,boolean remote){closed=true;messages.offer(new IOException("Relay connection ended"));}
            public void onError(Exception error){failure=error;closed=true;messages.offer(new IOException("Relay connection failed",error));}
            protected void onSetSSLParameters(SSLParameters parameters){parameters.setEndpointIdentificationAlgorithm("HTTPS");}
        };
        socket.setConnectionLostTimeout(30);
        try{if(!socket.connectBlocking(15,TimeUnit.SECONDS)||!socket.isOpen())throw new IOException("Server unavailable",failure);}
        catch(Exception ex){socket.closeConnection(1001,"Connection ended");throw ex;}
    }
    static RelayTransport open(String path)throws Exception{return new RelayTransport(path);}
    private void enqueue(Object value){
        try{if(!messages.offer(value,5,TimeUnit.SECONDS)){failure=new IOException("Connection cannot keep up");close();}}
        catch(InterruptedException ex){Thread.currentThread().interrupt();close();}
    }
    private Object next()throws IOException {
        if(closed&&messages.isEmpty())throw new IOException("Relay connection ended");
        try{Object message=messages.poll(readTimeout,TimeUnit.MILLISECONDS);if(message instanceof IOException)throw(IOException)message;if(message==null)throw new IOException(closed?"Relay connection ended":"Relay response timed out");return message;}
        catch(InterruptedException ex){Thread.currentThread().interrupt();throw new IOException("Connection interrupted",ex);}
    }
    String readText()throws IOException {Object value=next();if(!(value instanceof String))throw new IOException("Unexpected relay response");return(String)value;}
    void sendText(String value)throws IOException {if(value.length()>16384)throw new IOException("Relay request too large");synchronized(writeLock){waitWriter();socket.send(value);}}
    private void waitWriter()throws IOException {
        long end=System.nanoTime()+TimeUnit.SECONDS.toNanos(15);
        while(socket.hasBufferedData()){
            if(closed||System.nanoTime()>end)throw new IOException("Relay write timed out");
            try{Thread.sleep(2);}catch(InterruptedException ex){Thread.currentThread().interrupt();throw new IOException("Connection interrupted",ex);}
        }
        if(closed||!socket.isOpen())throw new IOException("Relay connection ended");
    }
    final InputStream input=new InputStream(){
        public int read()throws IOException{byte[] value=new byte[1];return read(value,0,1)==1?(value[0]&255):-1;}
        public int read(byte[] bytes,int offset,int count)throws IOException {
            if(count==0)return 0;
            if(pending==null||position==pending.length){Object value=next();if(!(value instanceof byte[]))throw new IOException("Unexpected relay packet");pending=(byte[])value;position=0;}
            int size=Math.min(count,pending.length-position);System.arraycopy(pending,position,bytes,offset,size);position+=size;return size;
        }
    };
    final OutputStream output=new OutputStream(){
        public void write(int value)throws IOException{write(new byte[]{(byte)value});}
        public void write(byte[] bytes,int offset,int count)throws IOException {
            synchronized(writeLock){while(count>0){int size=Math.min(65536,count);waitWriter();socket.send(Arrays.copyOfRange(bytes,offset,offset+size));offset+=size;count-=size;}}
        }
    };
    public void close(){closed=true;socket.closeConnection(1001,"Connection ended");messages.clear();messages.offer(new IOException("Relay connection ended"));}
}
