package com.android.support;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.text.Html;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

public class FloatingHUD {
    private static final String TAG = "FloatingHUD";

    private Context mContext;
    private WindowManager mWindowManager;
    private WindowManager.LayoutParams mParams;
    private FrameLayout mRootView;
    private LinearLayout mCard;

    // Counter TextViews
    private TextView tvGreen;
    private TextView tvRed;
    private TextView tvYellow;
    private TextView tvBlue;
    private TextView tvOrange;
    private TextView tvMiss;
    private TextView tvTotal;
    private TextView tvAcc;

    private Handler mHandler;
    private Runnable mUpdateRunnable;
    private boolean isVisible = false;
    private boolean isAdded = false;

    // Previous values cache to avoid redundant updates
    private int pGreen = -1, pRed = -1, pYellow = -1, pBlue = -1, pOrange = -1;
    private int pMiss = -1, pTotal = -1;

    public FloatingHUD(Context context) {
        this.mContext = context;
        this.mWindowManager = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);

        initLayoutParams();
        buildView();
        initUpdater();
        log("FloatingHUD initialized successfully");
    }

    @SuppressLint("WrongConstant")
    private void initLayoutParams() {
        int layoutType;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            layoutType = WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY; // 2038
        } else {
            layoutType = WindowManager.LayoutParams.TYPE_PHONE; // 2002
        }

        mParams = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                layoutType,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                        WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN |
                        WindowManager.LayoutParams.FLAG_SPLIT_TOUCH,
                PixelFormat.TRANSLUCENT
        );

        mParams.gravity = Gravity.TOP | Gravity.START;
        mParams.x = dp(16);
        mParams.y = dp(70);
    }

    private void buildView() {
        mRootView = new FrameLayout(mContext);

        // Card Container
        mCard = new LinearLayout(mContext);
        mCard.setOrientation(LinearLayout.VERTICAL);
        mCard.setPadding(dp(10), dp(8), dp(10), dp(8));

        // Dark sleek glassmorphism background with cyan accent border
        GradientDrawable bg = new GradientDrawable();
        bg.setShape(GradientDrawable.RECTANGLE);
        bg.setColor(Color.parseColor("#E0111A24")); // 88% dark slate
        bg.setCornerRadius(dp(10));
        bg.setStroke(dp(1.2f), Color.parseColor("#4D82CAFD")); // Subtle cyan stroke
        mCard.setBackground(bg);

        // Header (Title + Close Button)
        LinearLayout header = new LinearLayout(mContext);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView tvTitle = new TextView(mContext);
        tvTitle.setText(Html.fromHtml("<b><font color='#82CAFD'>⚡ NOTE HUD</font></b>"));
        tvTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        tvTitle.setTypeface(Typeface.DEFAULT_BOLD);
        tvTitle.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));

        TextView tvClose = new TextView(mContext);
        tvClose.setText(" ✕ ");
        tvClose.setTextColor(Color.parseColor("#B0BEC5"));
        tvClose.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        tvClose.setTypeface(Typeface.DEFAULT_BOLD);
        tvClose.setPadding(dp(4), dp(2), dp(4), dp(2));
        tvClose.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                hide();
            }
        });

        header.addView(tvTitle);
        header.addView(tvClose);
        mCard.addView(header);

        // Thin Separator line
        View divider1 = new View(mContext);
        divider1.setBackgroundColor(Color.parseColor("#26FFFFFF"));
        LinearLayout.LayoutParams divParams1 = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(1));
        divParams1.setMargins(0, dp(5), 0, dp(6));
        divider1.setLayoutParams(divParams1);
        mCard.addView(divider1);

        // Row 1: Green, Red, Yellow
        LinearLayout row1 = new LinearLayout(mContext);
        row1.setOrientation(LinearLayout.HORIZONTAL);
        row1.setGravity(Gravity.CENTER_VERTICAL);

        tvGreen = createBadgeView("🟢 G: 0", "#00E676");
        tvRed = createBadgeView("🔴 R: 0", "#FF5252");
        tvYellow = createBadgeView("🟡 Y: 0", "#FFEA00");

        row1.addView(tvGreen);
        row1.addView(tvRed);
        row1.addView(tvYellow);
        mCard.addView(row1);

        // Row 2: Blue, Orange, Miss
        LinearLayout row2 = new LinearLayout(mContext);
        row2.setOrientation(LinearLayout.HORIZONTAL);
        row2.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams row2Params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        row2Params.setMargins(0, dp(4), 0, dp(4));
        row2.setLayoutParams(row2Params);

        tvBlue = createBadgeView("🔵 B: 0", "#40C4FF");
        tvOrange = createBadgeView("🟠 O: 0", "#FF9100");
        tvMiss = createBadgeView("❌ Miss: 0", "#FF1744");

        row2.addView(tvBlue);
        row2.addView(tvOrange);
        row2.addView(tvMiss);
        mCard.addView(row2);

        // Thin Separator line
        View divider2 = new View(mContext);
        divider2.setBackgroundColor(Color.parseColor("#26FFFFFF"));
        LinearLayout.LayoutParams divParams2 = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(1));
        divParams2.setMargins(0, dp(2), 0, dp(4));
        divider2.setLayoutParams(divParams2);
        mCard.addView(divider2);

        // Row 3: Totals & Accuracy
        LinearLayout row3 = new LinearLayout(mContext);
        row3.setOrientation(LinearLayout.HORIZONTAL);
        row3.setGravity(Gravity.CENTER_VERTICAL);

        tvTotal = new TextView(mContext);
        tvTotal.setText(Html.fromHtml("🎯 Hit: <b><font color='#00E5FF'>0</font></b>"));
        tvTotal.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        tvTotal.setTextColor(Color.parseColor("#ECEFF1"));
        tvTotal.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));

        tvAcc = new TextView(mContext);
        tvAcc.setText(Html.fromHtml("Acc: <b><font color='#76FF03'>100%</font></b>"));
        tvAcc.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        tvAcc.setTextColor(Color.parseColor("#ECEFF1"));
        tvAcc.setGravity(Gravity.END);

        row3.addView(tvTotal);
        row3.addView(tvAcc);
        mCard.addView(row3);

        mRootView.addView(mCard);
        mRootView.setOnTouchListener(createTouchListener());
    }

    private TextView createBadgeView(String text, String colorHex) {
        TextView tv = new TextView(mContext);
        tv.setText(text);
        tv.setTextColor(Color.parseColor(colorHex));
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        tv.setTypeface(Typeface.DEFAULT_BOLD);
        tv.setPadding(dp(4), dp(2), dp(6), dp(2));
        return tv;
    }

    private View.OnTouchListener createTouchListener() {
        return new View.OnTouchListener() {
            private int initialX, initialY;
            private float touchX, touchY;
            private boolean isMoving = false;

            @Override
            public boolean onTouch(View v, MotionEvent event) {
                switch (event.getAction()) {
                    case MotionEvent.ACTION_DOWN:
                        initialX = mParams.x;
                        initialY = mParams.y;
                        touchX = event.getRawX();
                        touchY = event.getRawY();
                        isMoving = false;
                        return true;

                    case MotionEvent.ACTION_MOVE:
                        int deltaX = (int) (event.getRawX() - touchX);
                        int deltaY = (int) (event.getRawY() - touchY);
                        if (Math.abs(deltaX) > 5 || Math.abs(deltaY) > 5) {
                            isMoving = true;
                        }
                        mParams.x = initialX + deltaX;
                        mParams.y = initialY + deltaY;
                        try {
                            mWindowManager.updateViewLayout(mRootView, mParams);
                        } catch (Exception ignored) {
                        }
                        return true;

                    case MotionEvent.ACTION_UP:
                        return isMoving;
                }
                return false;
            }
        };
    }

    private void initUpdater() {
        mHandler = new Handler(Looper.getMainLooper());
        mUpdateRunnable = new Runnable() {
            @Override
            public void run() {
                if (isVisible) {
                    try {
                        int[] stats = Menu.GetNoteStats();
                        if (stats != null && stats.length >= 7) {
                            int green = stats[0];
                            int red = stats[1];
                            int yellow = stats[2];
                            int blue = stats[3];
                            int orange = stats[4];
                            int total = stats[5];
                            int miss = stats[6];

                            updateCounts(green, red, yellow, blue, orange, total, miss);
                        }
                    } catch (Throwable t) {
                        log("Error updating stats: " + t.getMessage());
                    }
                }
                mHandler.postDelayed(this, 100); // 10 FPS update rate
            }
        };
        mHandler.post(mUpdateRunnable);
    }

    public void updateCounts(int green, int red, int yellow, int blue, int orange, int total, int miss) {
        // Only redraw if any number changed
        if (green == pGreen && red == pRed && yellow == pYellow &&
                blue == pBlue && orange == pOrange && total == pTotal && miss == pMiss) {
            return;
        }

        pGreen = green;
        pRed = red;
        pYellow = yellow;
        pBlue = blue;
        pOrange = orange;
        pTotal = total;
        pMiss = miss;

        tvGreen.setText("🟢 G: " + green);
        tvRed.setText("🔴 R: " + red);
        tvYellow.setText("🟡 Y: " + yellow);
        tvBlue.setText("🔵 B: " + blue);
        tvOrange.setText("🟠 O: " + orange);
        tvMiss.setText(Html.fromHtml("❌ Miss: <b>" + miss + "</b>"));

        tvTotal.setText(Html.fromHtml("🎯 Hit: <b><font color='#00E5FF'>" + total + "</font></b>"));

        int allPlayed = total + miss;
        if (allPlayed > 0) {
            float acc = ((float) total / (float) allPlayed) * 100f;
            String accColor = acc >= 90 ? "#76FF03" : (acc >= 75 ? "#FFEA00" : "#FF5252");
            tvAcc.setText(Html.fromHtml(String.format(Locale.US, "Acc: <b><font color='%s'>%.1f%%</font></b>", accColor, acc)));
        } else {
            tvAcc.setText(Html.fromHtml("Acc: <b><font color='#76FF03'>100%</font></b>"));
        }
    }

    public void show() {
        if (!isAdded) {
            try {
                mWindowManager.addView(mRootView, mParams);
                isAdded = true;
            } catch (Exception e) {
                log("Error adding view to WindowManager: " + e.getMessage());
                return;
            }
        }
        mRootView.setVisibility(View.VISIBLE);
        isVisible = true;
        log("Floating HUD shown");
    }

    public void hide() {
        if (mRootView != null) {
            mRootView.setVisibility(View.GONE);
        }
        isVisible = false;
        log("Floating HUD hidden");
    }

    public boolean isShowing() {
        return isVisible;
    }

    private int dp(float value) {
        return (int) TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, value, mContext.getResources().getDisplayMetrics());
    }

    // =========================================================================
    // Lightweight Debug Logging System
    // Writes to /sdcard/0/Documents, /storage/emulated/0/Documents
    // =========================================================================
    public static void log(String message) {
        Log.i(TAG, message);
        writeToLogFile("[HUD] " + message);
    }

    public static void logCrash(String stackTrace) {
        Log.e(TAG, "CRASH: " + stackTrace);
        writeToLogFile("[CRASH] " + stackTrace);
    }

    public static synchronized void writeToLogFile(String text) {
        String[] candidateDirs = new String[]{
                "/sdcard/0/Documents",
                "/storage/emulated/0/Documents",
                "/sdcard/Documents"
        };

        String time = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date());
        String line = "[" + time + "] " + text + "\n";

        for (String dirPath : candidateDirs) {
            try {
                File dir = new File(dirPath);
                if (!dir.exists()) {
                    dir.mkdirs();
                }
                File logFile = new File(dir, "guitar_hero_debug.log");
                FileWriter fw = new FileWriter(logFile, true);
                fw.write(line);
                fw.flush();
                fw.close();
            } catch (Exception ignored) {
            }
        }
    }
}
