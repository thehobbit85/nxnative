package com.nxnative;
import android.os.Binder;
import android.os.Bundle;
import android.content.Intent;

/** Only framework delivery and process death are controlled; Activities are production code. */
public final class ProgramHandoffTest {
    private static void check(boolean c,String why){if(!c)throw new AssertionError(why);}
    private static GameActivity source(){
        GameActivity s=new GameActivity(); s.programHandoff(1,"Program 2");
        check(s.finished && s.started.size()==1,"source must leave the back stack after delivery");
        check(s.started.get(0).target==ProgramActivity.class,"handoff goes to the coordinator"); return s;
    }
    private static Binder lifetime(GameActivity s){return (Binder)s.started.get(0).getExtras().getBinder("process");}
    private static ProgramActivity coordinator(GameActivity s){
        ProgramActivity p=new ProgramActivity();p.intent=s.started.get(0);p.onCreate(null);return p;
    }
    private static void target(ProgramActivity p){
        check(p.finished && p.started.size()==1,"one target launch then coordinator finishes");
        Intent i=p.started.get(0);check(i.target==GameActivity.class && i.getIntExtra("previous",-1)==1,"previous index survives transition");
        check(Boolean.TRUE.equals(i.getExtras().values.get("handoff")) && (i.flags&Intent.FLAG_ACTIVITY_CLEAR_TOP)!=0,"fresh handoff intent");
    }
    public static void main(String[] args){
        GameActivity s=source();ProgramActivity p=coordinator(s);p.onResume();
        check(!p.finished && p.started.isEmpty(),"cannot start or expose Play while source is alive");
        lifetime(s).die();target(p);p.onResume();check(p.started.size()==1,"resume cannot duplicate a launch");p.onDestroy();

        s=source();lifetime(s).die();p=coordinator(s);check(p.started.isEmpty(),"already dead still waits for resume");p.onResume();target(p);p.onDestroy();

        s=source();p=coordinator(s);p.onResume();p.onPause();lifetime(s).die();
        check(!p.finished && p.started.isEmpty(),"background handoff must not launch an activity");p.onResume();target(p);p.onDestroy();

        s=source();p=coordinator(s);p.onResume();p.onBackPressed();
        check(!p.finished && p.started.isEmpty(),"Back cannot expose menu Play until old process exits");
        lifetime(s).die();check(p.finished && p.started.isEmpty(),"cancelled transition returns to menu without a target");p.onResume();check(p.started.isEmpty(),"cancel remains final");p.onDestroy();

        s=source();p=coordinator(s);p.onResume();p.onBackPressed();p.onPause();lifetime(s).die();
        check(!p.finished && p.started.isEmpty(),"background cancellation waits for foreground");p.onResume();
        check(p.finished && p.started.isEmpty(),"cancelled foreground return exposes menu only after death");p.onDestroy();

        for(int deathFirst=0;deathFirst<2;deathFirst++){
            s=source();p=coordinator(s);p.onResume();p.onBackPressed();p.onPause();
            Bundle state=new Bundle();p.saveState(state);p.onDestroy();
            if(deathFirst!=0)lifetime(s).die();
            p=new ProgramActivity();p.intent=s.started.get(0);p.onCreate(state);
            if(deathFirst==0)lifetime(s).die();
            p.onResume();check(p.finished && p.started.isEmpty(),"recreation must preserve cancellation");p.onDestroy();
        }

        GameActivity fail=new GameActivity();fail.failStart=true;
        try{fail.programHandoff(0,"x");throw new AssertionError("delivery failure lost");}catch(IllegalStateException expected){}
        check(!fail.finished && fail.started.isEmpty(),"failed delivery is not a completed transition");
        p=new ProgramActivity();p.intent=new Intent(null,ProgramActivity.class);p.onCreate(null);p.onResume();
        check(p.finished && p.started.isEmpty(),"missing lifetime cannot launch");p.onDestroy();
        System.out.println("PASS: handoff lifetime, cancellation, background/resume and delivery failures");
    }
}
