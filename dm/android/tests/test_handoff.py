"""Run the actual handoff Activities with a controlled Android framework boundary."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
STUBS = {
    'android/os/RemoteException.java': 'package android.os; public class RemoteException extends Exception {}',
    'android/os/IBinder.java': '''package android.os; public interface IBinder {
        interface DeathRecipient { void binderDied(); }
        void linkToDeath(DeathRecipient r, int f) throws RemoteException; boolean unlinkToDeath(DeathRecipient r, int f);
    }''',
    'android/os/Binder.java': '''package android.os; public class Binder implements IBinder {
        private boolean alive=true; private final java.util.List<DeathRecipient> listeners=new java.util.ArrayList<>();
        public void linkToDeath(DeathRecipient r,int f) throws RemoteException { if(!alive) throw new RemoteException(); listeners.add(r); }
        public boolean unlinkToDeath(DeathRecipient r,int f) { return listeners.remove(r); }
        public void die() { alive=false; for(DeathRecipient r:new java.util.ArrayList<>(listeners)) r.binderDied(); listeners.clear(); }
    }''',
    'android/os/Bundle.java': '''package android.os; public class Bundle {
        public final java.util.Map<String,Object> values=new java.util.HashMap<>();
        public void putBinder(String k,IBinder v){values.put(k,v);} public IBinder getBinder(String k){return (IBinder)values.get(k);}
        public void putInt(String k,int v){values.put(k,v);} public void putString(String k,String v){values.put(k,v);}
        public void putBoolean(String k,boolean v){values.put(k,v);} public boolean getBoolean(String k){return (Boolean)values.getOrDefault(k,false);}
    }''',
    'android/content/Intent.java': '''package android.content; import android.os.Bundle; public class Intent {
        public static final int FLAG_ACTIVITY_CLEAR_TOP=0x04000000; public Class<?> target; public int flags; private Bundle extras=new Bundle();
        public Intent(Object context,Class<?> t){target=t;} public Intent putExtras(Bundle b){extras=b;return this;}
        public Intent putExtra(String k,int v){extras.putInt(k,v);return this;} public Intent putExtra(String k,boolean v){extras.values.put(k,v);return this;}
        public Intent addFlags(int f){flags|=f;return this;} public Bundle getExtras(){return extras;}
        public String getStringExtra(String k){return (String)extras.values.get(k);}
        public int getIntExtra(String k,int d){return (Integer)extras.values.getOrDefault(k,d);}
    }''',
    'android/app/Activity.java': '''package android.app; import android.os.Bundle; import android.content.Intent; public class Activity {
        public final java.util.List<Intent> started=new java.util.ArrayList<>(); public Intent intent; public boolean finished,failStart;
        public Intent getIntent(){return intent;} public void runOnUiThread(Runnable r){r.run();}
        public void startActivity(Intent i){if(failStart)throw new IllegalStateException("start failed");started.add(i);}
        public void finish(){finished=true;} public boolean isFinishing(){return finished;} public void setContentView(Object v){}
        protected void onCreate(Bundle b){} protected void onResume(){} protected void onPause(){} protected void onDestroy(){}
        protected void onSaveInstanceState(Bundle b){}
        public void saveState(Bundle b){onSaveInstanceState(b);}
        public void onBackPressed(){finish();}
    }''',
    'android/app/NativeActivity.java': 'package android.app; public class NativeActivity extends Activity {}',
    'android/view/Gravity.java': 'package android.view; public final class Gravity { public static final int CENTER=17; }',
    'android/widget/TextView.java': '''package android.widget; public class TextView {
        public TextView(Object c){} public void setTextSize(int n){} public void setGravity(int n){} public void setText(String s){}
    }''',
}


class Handoff(unittest.TestCase):
    def test_lifetime_background_cancellation_and_delivery(self):
        home = os.environ.get('JAVA_HOME', '')
        javac = str(Path(home) / 'bin/javac') if home else shutil.which('javac')
        java = str(Path(home) / 'bin/java') if home else shutil.which('java')
        if not javac or not java: self.skipTest('JDK required for the Java lifecycle boundary fixture')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); sources = []
            for name, source in STUBS.items():
                path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(source); sources.append(path)
            app = ROOT / 'dm/android/app/java/com/nxnative'
            sources += [app / 'GameActivity.java', app / 'ProgramActivity.java', Path(__file__).parent / 'handoff/ProgramHandoffTest.java']
            compiled = subprocess.run([javac, '--release', '11', '-d', str(root), *map(str, sources)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([java, '-cp', str(root), 'com.nxnative.ProgramHandoffTest'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS: handoff lifetime, cancellation, background/resume and delivery failures', result.stdout)


if __name__ == '__main__': unittest.main()
