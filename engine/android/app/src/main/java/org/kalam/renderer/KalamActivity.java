package org.kalam.renderer;

import android.content.Context;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

public final class KalamActivity extends SDLActivity {
    private static native void publishSurface(Surface surface);

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new SDLSurface(context) {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                publishSurface(holder.getSurface());
                super.surfaceCreated(holder);
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                publishSurface(null);
                super.surfaceDestroyed(holder);
            }
        };
    }

    @Override
    protected String[] getArguments() {
        java.util.ArrayList<String> args = new java.util.ArrayList<>();
        if (getIntent().getBooleanExtra("gles", false)) {
            args.add("--gles");
        }
        String runId = getIntent().getStringExtra("run_id");
        if (getIntent().getBooleanExtra("capture", false) && runId != null && runId.matches("[a-f0-9]{32}")) {
            args.add("--capture");
            args.add(new java.io.File(getFilesDir(), "capture-" + runId).getPath());
        } else if (getIntent().getBooleanExtra("smoke", false)) {
            args.add("--smoke");
        }
        return args.toArray(new String[0]);
    }

    @Override
    protected void main() {
        int status = SDLActivity.nativeRunMain(getMainSharedObject(), getMainFunction(), getArguments());
        Log.i("Kalam", "native exit " + status + " run=" + getIntent().getStringExtra("run_id"));
    }
}
