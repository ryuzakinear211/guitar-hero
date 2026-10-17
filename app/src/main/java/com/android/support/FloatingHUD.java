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

import android.text.TextUtils;
import org.json.JSONObject;

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
    private TextView tvCombo;

    // Chart Detail Views
    private LinearLayout mChartSection;
    private TextView tvChartTitle;
    private TextView tvChartMeta;
    private TextView tvChartSp;
    private TextView tvChartScore;
    private TextView tvChartPathNext;
    private TextView tvChartPathRoute;
    private TextView tvToggleChart;
    private boolean isChartExpanded = true;
    private int chartUpdateTick = 0;

    private Handler mHandler;
    private Runnable mUpdateRunnable;
    private boolean isVisible = false;
    private boolean isAdded = false;

    // Previous values cache to avoid redundant updates
    private int pGreen = -1, pRed = -1, pYellow = -1, pBlue = -1, pOrange = -1;
    private int pMiss = -1, pTotal = -1;
    private int pCombo = -1;
    private int pScore = -1;
    private String pChartJson = "";

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
        mCard.setMinimumWidth(dp(220));

        // Dark sleek glassmorphism background with cyan accent border
        GradientDrawable bg = new GradientDrawable();
        bg.setShape(GradientDrawable.RECTANGLE);
        bg.setColor(Color.parseColor("#E60F1722")); // 90% dark slate
        bg.setCornerRadius(dp(10));
        bg.setStroke(dp(1.2f), Color.parseColor("#4D00E5FF")); // Subtle cyan stroke
        mCard.setBackground(bg);

        // Header (Title + Toggle Chart + Close Button)
        LinearLayout header = new LinearLayout(mContext);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        tvTitle = new TextView(mContext);
        tvTitle.setText(Html.fromHtml("<b><font color='#00E5FF'>⚡ NOTE & CHART HUD</font></b>"));
        tvTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        tvTitle.setTypeface(Typeface.DEFAULT_BOLD);
        tvTitle.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));

        tvToggleChart = new TextView(mContext);
        tvToggleChart.setText(" ▴ ");
        tvToggleChart.setTextColor(Color.parseColor("#80D8FF"));
        tvToggleChart.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        tvToggleChart.setTypeface(Typeface.DEFAULT_BOLD);
        tvToggleChart.setPadding(dp(6), dp(2), dp(6), dp(2));
        tvToggleChart.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                isChartExpanded = !isChartExpanded;
                if (mChartSection != null) {
                    mChartSection.setVisibility(isChartExpanded ? View.VISIBLE : View.GONE);
                }
                tvToggleChart.setText(isChartExpanded ? " ▴ " : " ▾ ");
            }
        });

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
        header.addView(tvToggleChart);
        header.addView(tvClose);
        mCard.addView(header);

        // =====================================================================
        // Chart Details Section (Collapsible, Sleek Inset Container)
        // =====================================================================
        mChartSection = new LinearLayout(mContext);
        mChartSection.setOrientation(LinearLayout.VERTICAL);
        mChartSection.setPadding(dp(7), dp(6), dp(7), dp(6));
        LinearLayout.LayoutParams chartSecParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        chartSecParams.setMargins(0, dp(4), 0, dp(4));
        mChartSection.setLayoutParams(chartSecParams);

        GradientDrawable chartBg = new GradientDrawable();
        chartBg.setShape(GradientDrawable.RECTANGLE);
        chartBg.setColor(Color.parseColor("#590B131D")); // Sleek dark card inset
        chartBg.setCornerRadius(dp(6));
        chartBg.setStroke(dp(1), Color.parseColor("#3300E5FF")); // Subtle neon cyan accent
        mChartSection.setBackground(chartBg);

        // 1. Judul Lagu (Song Title)
        tvChartTitle = new TextView(mContext);
        tvChartTitle.setText(Html.fromHtml("🎵 <i><font color='#90A4AE'>Menunggu Lagu Dimulai...</font></i>"));
        tvChartTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        tvChartTitle.setTypeface(Typeface.DEFAULT_BOLD);
        tvChartTitle.setSingleLine(true);
        tvChartTitle.setEllipsize(TextUtils.TruncateAt.END);
        mChartSection.addView(tvChartTitle);

        // 2. Durasi Lagu & Total Notes
        tvChartMeta = new TextView(mContext);
        tvChartMeta.setText(Html.fromHtml("⏱️ <font color='#90A4AE'>--:-- • 0 Notes</font>"));
        tvChartMeta.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9.5f);
        tvChartMeta.setPadding(0, dp(2), 0, 0);
        mChartSection.addView(tvChartMeta);

        // 3. Total Note Special (Energi) & Frase
        tvChartSp = new TextView(mContext);
        tvChartSp.setText(Html.fromHtml("⭐ Note SP: <font color='#90A4AE'>0 (0 Frase)</font>"));
        tvChartSp.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9.5f);
        tvChartSp.setPadding(0, dp(2), 0, 0);
        mChartSection.addView(tvChartSp);

        // 4. Skor Maksimal (Best Potential Score)
        tvChartScore = new TextView(mContext);
        tvChartScore.setText(Html.fromHtml("🏆 Skor Maks: <font color='#90A4AE'>-- pts</font>"));
        tvChartScore.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9.5f);
        tvChartScore.setTypeface(Typeface.DEFAULT_BOLD);
        tvChartScore.setPadding(0, dp(2), 0, 0);
        mChartSection.addView(tvChartScore);

        // 5. Next Combo Trigger Target
        tvChartPathNext = new TextView(mContext);
        tvChartPathNext.setText(Html.fromHtml("🎯 Trigger Kombo: <font color='#90A4AE'>--</font>"));
        tvChartPathNext.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9.5f);
        tvChartPathNext.setTypeface(Typeface.DEFAULT_BOLD);
        tvChartPathNext.setPadding(0, dp(2), 0, 0);
        mChartSection.addView(tvChartPathNext);

        // 6. Optimal Route Summary
        tvChartPathRoute = new TextView(mContext);
        tvChartPathRoute.setText(Html.fromHtml("📋 Rute: <font color='#90A4AE'>--</font>"));
        tvChartPathRoute.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9f);
        tvChartPathRoute.setPadding(0, dp(1), 0, 0);
        mChartSection.addView(tvChartPathRoute);

        mCard.addView(mChartSection);

        // Thin Separator line
        View divider1 = new View(mContext);
        divider1.setBackgroundColor(Color.parseColor("#26FFFFFF"));
        LinearLayout.LayoutParams divParams1 = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(1));
        divParams1.setMargins(0, dp(4), 0, dp(5));
        divider1.setLayoutParams(divParams1);
        mCard.addView(divider1);

        // Row 1: Green, Red, Yellow (equal width 1.0f weight to prevent clipping)
        LinearLayout row1 = new LinearLayout(mContext);
        row1.setOrientation(LinearLayout.HORIZONTAL);
        row1.setGravity(Gravity.CENTER_VERTICAL);
        row1.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        tvGreen = createBadgeView("🟢 G: 0", "#00E676");
        tvRed = createBadgeView("🔴 R: 0", "#FF5252");
        tvYellow = createBadgeView("🟡 Y: 0", "#FFEA00");

        LinearLayout.LayoutParams badgeParams1 = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f);
        tvGreen.setLayoutParams(badgeParams1);
        tvRed.setLayoutParams(badgeParams1);
        tvYellow.setLayoutParams(badgeParams1);

        row1.addView(tvGreen);
        row1.addView(tvRed);
        row1.addView(tvYellow);
        mCard.addView(row1);

        // Row 2: Blue, Orange, Miss (equal width 1.0f weight)
        LinearLayout row2 = new LinearLayout(mContext);
        row2.setOrientation(LinearLayout.HORIZONTAL);
        row2.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams row2Params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        row2Params.setMargins(0, dp(4), 0, dp(4));
        row2.setLayoutParams(row2Params);

        tvBlue = createBadgeView("🔵 B: 0", "#40C4FF");
        tvOrange = createBadgeView("🟠 O: 0", "#FF9100");
        tvMiss = createBadgeView("❌ Miss: 0", "#FF1744");

        LinearLayout.LayoutParams badgeParams2 = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f);
        tvBlue.setLayoutParams(badgeParams2);
        tvOrange.setLayoutParams(badgeParams2);
        tvMiss.setLayoutParams(badgeParams2);

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

        // Thin Separator line
        View divider3 = new View(mContext);
        divider3.setBackgroundColor(Color.parseColor("#26FFFFFF"));
        LinearLayout.LayoutParams divParams3 = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(1));
        divParams3.setMargins(0, dp(4), 0, dp(4));
        divider3.setLayoutParams(divParams3);
        mCard.addView(divider3);

        // Row 4: Live Combo & Live Score
        LinearLayout row4 = new LinearLayout(mContext);
        row4.setOrientation(LinearLayout.HORIZONTAL);
        row4.setGravity(Gravity.CENTER_VERTICAL);
        row4.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        tvCombo = new TextView(mContext);
        tvCombo.setText(Html.fromHtml("🔥 Combo: <b><font color='#FFD700'>0</font></b>"));
        tvCombo.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        tvCombo.setTextColor(Color.parseColor("#ECEFF1"));
        tvCombo.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        row4.addView(tvCombo);
        mCard.addView(row4);

        mRootView.addView(mCard);
        mRootView.setOnTouchListener(createTouchListener());
    }

    private TextView createBadgeView(String text, String colorHex) {
        TextView tv = new TextView(mContext);
        tv.setText(text);
        tv.setTextColor(Color.parseColor(colorHex));
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        tv.setTypeface(Typeface.DEFAULT_BOLD);
        tv.setSingleLine(true);
        tv.setPadding(dp(2), dp(2), dp(2), dp(2));
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

    private TextView tvTitle;
    private int pAuto = -1;
    private int pAutoMaster = -1;
    private int pTargetScore = 0;

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
                            int autoHits = stats.length >= 9 ? stats[8] : 0;
                            int autoMaster = stats.length >= 10 ? stats[9] : 0;
                            int combo = stats.length >= 11 ? stats[10] : 0;
                            int score = stats.length >= 12 ? stats[11] : 0;

                            updateCounts(green, red, yellow, blue, orange, total, miss, autoHits, autoMaster, combo, score);
                        }

                        // Periodic chart detail updates (every 3 ticks = ~300ms)
                        if (++chartUpdateTick >= 3) {
                            chartUpdateTick = 0;
                            updateChartDetails();
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

    public void updateChartDetails() {
        try {
            String jsonStr = Menu.GetChartDetails();
            if (jsonStr == null || jsonStr.equals(pChartJson)) {
                return;
            }
            pChartJson = jsonStr;

            JSONObject obj = new JSONObject(jsonStr);
            boolean hasData = obj.optBoolean("hasData", false);
            String title = obj.optString("title", "Menunggu Lagu...");
            String durationStr = obj.optString("durationStr", "--:--");
            int totalNotes = obj.optInt("totalNotes", 0);
            int spNotes = obj.optInt("spNotes", 0);
            int spPhrases = obj.optInt("spPhrases", 0);
            long bestScore = obj.optLong("bestScore", 0);
            long baseScore = obj.optLong("baseScore", 0);
            String pathSummary = obj.optString("pathSummary", "-");
            int stepIndex = obj.optInt("stepIndex", 0);
            int stepTotal = obj.optInt("stepTotal", 0);
            int nextTargetHit = obj.optInt("nextTargetHit", 0);
            int nextReqEnergy = obj.optInt("nextReqEnergy", 0);
            int targetScore = obj.optInt("targetScore", 0);
            long predictedScore = obj.optLong("predictedScore", bestScore);
            String targetStatus = obj.optString("targetStatus", "AUTO MAX");
            pTargetScore = targetScore;

            if (hasData) {
                if (tvChartTitle != null) {
                    tvChartTitle.setText(Html.fromHtml("🎵 <b><font color='#FFFFFF'>" + title + "</font></b>"));
                }
                if (tvChartMeta != null) {
                    tvChartMeta.setText(Html.fromHtml("⏱️ <font color='#00E5FF'><b>" + durationStr + "</b></font> <font color='#90A4AE'>•</font> 🎯 <font color='#82CAFD'><b>" + totalNotes + " Notes</b></font>"));
                }
                if (tvChartSp != null) {
                    tvChartSp.setText(Html.fromHtml("⭐ Note SP: <b><font color='#E040FB'>" + spNotes + "</font></b> <small><font color='#B388FF'>(" + spPhrases + " Frase / Energi)</font></small>"));
                }
                if (tvChartScore != null) {
                    String predStr = String.format(Locale.US, "%,d", predictedScore);
                    String maxStr = String.format(Locale.US, "%,d", bestScore);
                    if (targetScore > 0) {
                        String targetStr = String.format(Locale.US, "%,d", targetScore);
                        String statusColor = targetStatus.contains("DEFICIT") ? "#FF9100" : (targetStatus.contains("TERCAPAI") ? "#00E676" : "#76FF03");
                        tvChartScore.setText(Html.fromHtml("🎯 Target: <b><font color='#00E5FF'>" + targetStr + "</font></b> • 🔮 Prediksi: <b><font color='" + statusColor + "'>" + predStr + "</font></b><br><small><font color='" + statusColor + "'>[" + targetStatus + "]</font> <font color='#B0BEC5'>(Maks: " + maxStr + ")</font></small>"));
                    } else {
                        if (baseScore > 0) {
                            tvChartScore.setText(Html.fromHtml("🏆 Skor Maks: <b><font color='#FFD700'>" + maxStr + "</font></b> • 🔮 Prediksi: <b><font color='#76FF03'>" + predStr + "</font></b><br><small><font color='#B0BEC5'>(FC: " + String.format(Locale.US, "%,d", baseScore) + ")</font></small>"));
                        } else {
                            tvChartScore.setText(Html.fromHtml("🏆 Skor Maks: <b><font color='#FFD700'>" + maxStr + "</font></b> • 🔮 Prediksi: <b><font color='#76FF03'>" + predStr + "</font></b>"));
                        }
                    }
                }
                if (tvChartPathNext != null) {
                    if (stepTotal > 0) {
                        if (stepIndex < stepTotal) {
                            tvChartPathNext.setText(Html.fromHtml("🎯 Trigger Kombo: <b><font color='#76FF03'>#" + nextTargetHit + "</font></b> <small><font color='#00E5FF'>(" + nextReqEnergy + "⚡ Energi)</font> <font color='#FFD700'>[Step " + (stepIndex + 1) + "/" + stepTotal + "]</font></small>"));
                        } else {
                            tvChartPathNext.setText(Html.fromHtml("🎯 Trigger Kombo: <b><font color='#76FF03'>Selesai ✅</font></b> <small><font color='#FFD700'>[" + stepTotal + "/" + stepTotal + "]</font></small>"));
                        }
                    } else {
                        tvChartPathNext.setText(Html.fromHtml("🎯 Trigger Kombo: <font color='#B0BEC5'>Dinamis Real-Time</font>"));
                    }
                }
                if (tvChartPathRoute != null) {
                    tvChartPathRoute.setText(Html.fromHtml("📋 Rute: <font color='#80D8FF'><b>" + pathSummary + "</b></font>"));
                }
            } else {
                if (tvChartTitle != null) {
                    tvChartTitle.setText(Html.fromHtml("🎵 <i><font color='#90A4AE'>Menunggu Lagu Dimulai...</font></i>"));
                }
                if (tvChartMeta != null) {
                    tvChartMeta.setText(Html.fromHtml("⏱️ <font color='#90A4AE'>--:-- • 0 Notes</font>"));
                }
                if (tvChartSp != null) {
                    tvChartSp.setText(Html.fromHtml("⭐ Note SP: <font color='#90A4AE'>0 (0 Frase)</font>"));
                }
                if (tvChartScore != null) {
                    tvChartScore.setText(Html.fromHtml("🏆 Skor Maks: <font color='#90A4AE'>-- pts</font>"));
                }
                if (tvChartPathNext != null) {
                    tvChartPathNext.setText(Html.fromHtml("🎯 Trigger Kombo: <font color='#90A4AE'>--</font>"));
                }
                if (tvChartPathRoute != null) {
                    tvChartPathRoute.setText(Html.fromHtml("📋 Rute: <font color='#90A4AE'>--</font>"));
                }
            }
        } catch (Exception e) {
            log("Error updating chart details: " + e.getMessage());
        }
    }

    public void updateCounts(int green, int red, int yellow, int blue, int orange, int total, int miss, int autoHits, int autoMaster,
                             int combo, int score) {
        // Only redraw if any number changed
        if (green == pGreen && red == pRed && yellow == pYellow &&
                blue == pBlue && orange == pOrange && total == pTotal &&
                miss == pMiss && autoHits == pAuto && autoMaster == pAutoMaster &&
                combo == pCombo && score == pScore) {
            return;
        }

        pGreen = green;
        pRed = red;
        pYellow = yellow;
        pBlue = blue;
        pOrange = orange;
        pTotal = total;
        pMiss = miss;
        pAuto = autoHits;
        pAutoMaster = autoMaster;
        pCombo = combo;
        pScore = score;

        tvGreen.setText("🟢 G: " + green);
        tvRed.setText("🔴 R: " + red);
        tvYellow.setText("🟡 Y: " + yellow);
        tvBlue.setText("🔵 B: " + blue);
        tvOrange.setText("🟠 O: " + orange);
        tvMiss.setText(Html.fromHtml("❌ Miss: <b>" + miss + "</b>"));

        // When Auto Play is active, autoHits is completely synchronized with total hit count
        int displayAutoHits = (autoMaster == 1 || autoHits >= total) ? total : autoHits;
        if (displayAutoHits > 0) {
            tvTotal.setText(Html.fromHtml("🎯 Hit: <b><font color='#00E5FF'>" + total + "</font></b> <small><font color='#76FF03'>(" + displayAutoHits + " auto)</font></small>"));
        } else {
            tvTotal.setText(Html.fromHtml("🎯 Hit: <b><font color='#00E5FF'>" + total + "</font></b>"));
        }

        if (tvTitle != null) {
            if (autoMaster == 1) {
                tvTitle.setText(Html.fromHtml("<b><font color='#00E5FF'>⚡ NOTE & CHART HUD</font> <font color='#00E676'><small>[AUTO]</small></font></b>"));
            } else {
                tvTitle.setText(Html.fromHtml("<b><font color='#00E5FF'>⚡ NOTE & CHART HUD</font></b>"));
            }
        }

        // Live Combo & Score display
        String scoreFormatted = String.format(Locale.US, "%,d", score);
        String targetPart = (pTargetScore > 0) ? " <small><font color='#B0BEC5'>/ " + String.format(Locale.US, "%,d", pTargetScore) + "</font></small>" : "";
        if (combo >= 30) {
            tvCombo.setText(Html.fromHtml("🔥 Combo: <b><font color='#FFD700'>" + combo + "</font></b> <small><font color='#00E676'>(4x)</font></small> <font color='#90A4AE'>•</font> 🎯 Skor: <b><font color='#00E5FF'>" + scoreFormatted + "</font></b>" + targetPart));
        } else {
            tvCombo.setText(Html.fromHtml("🔥 Combo: <b><font color='#FFD700'>" + combo + "</font></b> <font color='#90A4AE'>•</font> 🎯 Skor: <b><font color='#00E5FF'>" + scoreFormatted + "</font></b>" + targetPart));
        }

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
