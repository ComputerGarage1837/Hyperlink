package ca.myfamilyapps.hyperlink;

import java.io.IOException;
import java.util.concurrent.CompletableFuture;

public final class ReplyInboxCheck {
    private static String id(int value){return String.format("%032x",value);}
    private static void check(boolean condition,String label){if(!condition)throw new AssertionError(label);}
    public static void main(String[] args) throws Exception {
        ReplyInbox inbox=new ReplyInbox();
        CompletableFuture<byte[]> first=inbox.register(id(1)),second=inbox.register(id(2));
        inbox.accept(id(99),new byte[]{9});check(!first.isDone() && !second.isDone(),"Unknown replies must not resolve requests");
        inbox.accept(id(2),new byte[]{2});check(!first.isDone() && second.isDone(),"Replies must match their request identity");
        check(inbox.await(id(2),second)[0]==2,"Correlated reply payload");
        try{inbox.register(id(1));throw new AssertionError("Duplicate request accepted");}catch(IOException expected){}
        CompletableFuture<byte[]> third=inbox.register(id(3));inbox.register(id(4));inbox.register(id(5));
        try{inbox.register(id(6));throw new AssertionError("Unbounded requests");}catch(IOException expected){}
        inbox.close();check(first.isCompletedExceptionally() && third.isCompletedExceptionally(),"Disconnect must wake outstanding requests");
        try{inbox.await(id(1),first);throw new AssertionError("Disconnected request succeeded");}catch(IOException expected){}
        try{inbox.register(id(7));throw new AssertionError("Closed inbox accepted request");}catch(IOException expected){}
        System.out.println("7 Android request correlation checks passed");
    }
}
