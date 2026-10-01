package ca.myfamilyapps.hyperlink;

import java.io.*;
import java.nio.ByteBuffer;
import javax.net.ssl.*;

/** JSSE TLS 1.2, certificate-pinned end to end through the opaque relay. */
final class RelayTls implements Closeable, PinnedWire.TimeoutTarget {
    private final RelayTransport transport;
    private final SSLEngine engine;
    private final ByteBuffer incoming=ByteBuffer.allocate(65536),plain=ByteBuffer.allocate(65536),outgoing=ByteBuffer.allocate(65536);
    private final Object writeLock=new Object();
    private volatile boolean closed;
    public void timeout(int milliseconds){if(milliseconds<1)throw new IllegalArgumentException("Invalid read timeout");transport.readTimeout=milliseconds;}
    RelayTls(RelayTransport connection,String fingerprint)throws Exception {
        transport=connection;engine=PinnedWire.pinnedContext(fingerprint).createSSLEngine("Hyperlink",0);engine.setUseClientMode(true);engine.setEnabledProtocols(new String[]{"TLSv1.2"});
        incoming.limit(0);plain.limit(0);
        try{handshake();transport.readTimeout=120000;}catch(Exception error){close();throw error;}
    }
    private void record()throws IOException {
        DataInputStream stream=new DataInputStream(transport.input);byte[] header=new byte[5];stream.readFully(header);
        int size=((header[3]&255)<<8)|(header[4]&255);
        if((header[0]&255)<20||(header[0]&255)>23||header[1]!=3||size<1||size>18432)throw new IOException("Invalid TLS record");
        incoming.compact();if(incoming.remaining()<size+5)throw new IOException("TLS input overflow");incoming.put(header);
        byte[] payload=new byte[size];stream.readFully(payload);incoming.put(payload);incoming.flip();
    }
    private void emit()throws IOException {
        outgoing.flip();byte[] packet=new byte[outgoing.remaining()];outgoing.get(packet);if(packet.length>0)transport.output.write(packet);
    }
    private void tasks(){Runnable task;while((task=engine.getDelegatedTask())!=null)task.run();}
    private void handshake()throws IOException {
        engine.beginHandshake();ByteBuffer empty=ByteBuffer.allocate(0);ByteBuffer scratch=ByteBuffer.allocate(65536);
        long deadline=System.nanoTime()+java.util.concurrent.TimeUnit.SECONDS.toNanos(30);
        while(engine.getHandshakeStatus()!=SSLEngineResult.HandshakeStatus.NOT_HANDSHAKING){
            if(System.nanoTime()>deadline)throw new IOException("Host TLS handshake timed out");
            switch(engine.getHandshakeStatus()){
                case NEED_TASK:tasks();break;
                case NEED_WRAP:
                    outgoing.clear();SSLEngineResult wrapped=engine.wrap(empty,outgoing);if(wrapped.getStatus()!=SSLEngineResult.Status.OK)throw new IOException("Host TLS handshake failed");emit();break;
                case NEED_UNWRAP:
                    if(!incoming.hasRemaining())record();scratch.clear();SSLEngineResult unwrapped=engine.unwrap(incoming,scratch);
                    if(unwrapped.getStatus()==SSLEngineResult.Status.BUFFER_UNDERFLOW)record();
                    else if(unwrapped.getStatus()!=SSLEngineResult.Status.OK||unwrapped.bytesProduced()!=0)throw new IOException("Invalid host TLS handshake");
                    break;
                default:throw new IOException("Unsupported host TLS handshake");
            }
        }
    }
    final InputStream input=new InputStream(){
        public int read()throws IOException{byte[] value=new byte[1];return read(value,0,1)==1?(value[0]&255):-1;}
        public int read(byte[] bytes,int offset,int count)throws IOException {
            if(count==0)return 0;if(closed)throw new IOException("Host connection ended");
            while(!plain.hasRemaining()){
                plain.clear();if(!incoming.hasRemaining())record();SSLEngineResult result=engine.unwrap(incoming,plain);
                if(result.getStatus()==SSLEngineResult.Status.BUFFER_UNDERFLOW){plain.limit(0);record();continue;}
                if(result.getStatus()!=SSLEngineResult.Status.OK||result.getHandshakeStatus()!=SSLEngineResult.HandshakeStatus.NOT_HANDSHAKING)throw new IOException("Host TLS connection ended or attempted renegotiation");
                plain.flip();if(result.bytesConsumed()==0&&result.bytesProduced()==0)throw new IOException("TLS reader stalled");
            }
            int size=Math.min(count,plain.remaining());plain.get(bytes,offset,size);return size;
        }
    };
    final OutputStream output=new OutputStream(){
        public void write(int value)throws IOException{write(new byte[]{(byte)value});}
        public void write(byte[] bytes,int offset,int count)throws IOException {
            synchronized(writeLock){
                if(closed)throw new IOException("Host connection ended");ByteBuffer source=ByteBuffer.wrap(bytes,offset,count);
                while(source.hasRemaining()){
                    outgoing.clear();SSLEngineResult result=engine.wrap(source,outgoing);
                    if(result.getStatus()!=SSLEngineResult.Status.OK||result.getHandshakeStatus()!=SSLEngineResult.HandshakeStatus.NOT_HANDSHAKING||result.bytesConsumed()==0)throw new IOException("Host TLS writer stalled");emit();
                }
            }
        }
    };
    public void close(){closed=true;transport.close();}
}
