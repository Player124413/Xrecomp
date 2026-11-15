package fr.adserver.xrecomp;

import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Insets;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RadialGradient;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.drawable.Drawable;
import android.os.SystemClock;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.text.TextUtils;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.LinearLayout;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.view.WindowInsets;

/**
 * The launcher's custom views and drawables (XBA-41), drawn on Canvas in design pixels (Theme). Framework classes
 * only: no AndroidX, no images, no font files.
 */
final class Widgets {
    private Widgets() {}

    private static Paint fill(int color) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setStyle(Paint.Style.FILL);
        p.setColor(color);
        return p;
    }

    private static Paint stroke(int color, float width) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(width);
        p.setStrokeCap(Paint.Cap.ROUND);
        p.setStrokeJoin(Paint.Join.ROUND);
        p.setColor(color);
        return p;
    }

    private static TextPaint textPaint(float size, android.graphics.Typeface face, int color) {
        TextPaint p = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        p.setTextSize(size);
        p.setTypeface(face);
        p.setColor(color);
        return p;
    }

    /**
     * s cut to avail pixels with « … », measured with p itself (Paint.breakText): TextUtils.ellipsize let a long
     * French setting title run under its value (the U1 bug), so the rows do not rely on it.
     */
    static String fit(String s, Paint p, float avail) {
        if (s == null || s.isEmpty() || p.measureText(s) <= avail)
            return s == null ? "" : s;
        float room = Math.max(0, avail - p.measureText("…"));
        int n = p.breakText(s, true, room, null);
        return n <= 0 ? "…" : s.substring(0, n).trim() + "…";
    }

    private static int blend(int a, int b, float t) {
        t = Math.max(0, Math.min(1, t));
        int aa = a >>> 24, ar = a >> 16 & 0xff, ag = a >> 8 & 0xff, ab = a & 0xff;
        int ba = b >>> 24, br = b >> 16 & 0xff, bg = b >> 8 & 0xff, bb = b & 0xff;
        return Math.round(aa + (ba - aa) * t) << 24 | Math.round(ar + (br - ar) * t) << 16
               | Math.round(ag + (bg - ag) * t) << 8 | Math.round(ab + (bb - ab) * t);
    }

    // ---- Stage: the 1920 x 1080 design grid, scaled to the screen ----------------------------------------------

    /**
     * Holds the launcher's content at design size (at least 1920 x 1080, wider or taller to match the screen's
     * shape) and scales it to fill the window less the display cutout. Touch and focus go through the transform.
     */
    static final class Stage extends ViewGroup {
        private final View content;
        private Insets cut = Insets.NONE;
        private float scale = 1;
        private int dw = Theme.DESIGN_W, dh = Theme.DESIGN_H;

        Stage(Context c, View content) {
            super(c);
            this.content = content;
            addView(content);
            setOnApplyWindowInsetsListener((v, in) -> {
                Insets i = in.getInsets(WindowInsets.Type.displayCutout());
                if (!i.equals(cut)) {
                    cut = i;
                    requestLayout();
                }
                return WindowInsets.CONSUMED;
            });
        }

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getSize(wSpec), h = MeasureSpec.getSize(hSpec);
            int aw = Math.max(1, w - cut.left - cut.right), ah = Math.max(1, h - cut.top - cut.bottom);
            scale = Math.min(aw / (float)Theme.DESIGN_W, ah / (float)Theme.DESIGN_H);
            dw = Math.round(aw / scale);
            dh = Math.round(ah / scale);
            content.measure(MeasureSpec.makeMeasureSpec(dw, MeasureSpec.EXACTLY),
                            MeasureSpec.makeMeasureSpec(dh, MeasureSpec.EXACTLY));
            setMeasuredDimension(w, h);
        }

        @Override
        protected void onLayout(boolean changed, int l, int t, int r, int b) {
            content.layout(0, 0, dw, dh);
            content.setPivotX(0);
            content.setPivotY(0);
            content.setScaleX(scale);
            content.setScaleY(scale);
            content.setTranslationX(cut.left);
            content.setTranslationY(cut.top);
        }
    }

    // ---- Backgrounds ---------------------------------------------------------------------------------------------

    private abstract static class SimpleDrawable extends Drawable {
        @Override
        public void setAlpha(int alpha) {}

        @Override
        public void setColorFilter(ColorFilter cf) {}

        /** Abstract in Drawable, so it has to be there, though the framework no longer reads it. */
        @Override
        @Deprecated
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }
    }

    /**
     * The screen behind everything: a dark gradient, and either the game theme's hero (its title image, tiny, blurred
     * and luminance-capped by GamePalette.bakeHero) washing in from the right — Home and Preparing only — or one soft
     * glow (the accent, or the game's GLOW). The shaders are made once per size and colours, not per frame.
     */
    static final class Backdrop extends SimpleDrawable {
        /** The glow's opacity (only without a hero: the hero is the light then). */
        static final float GLOW_ALPHA = 0.07f;
        /** The hero's opacity at its strongest. */
        static final float HERO_ALPHA = 0.85f;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint heroP = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.DITHER_FLAG);
        private int glow = Theme.GLOW;
        private android.graphics.Bitmap hero;
        private Shader bg, glowS, heroS;
        private int shaderW = -1, shaderH = -1;

        Backdrop() {
            paint.setDither(true);
        }

        /** The glow's colour (the game theme's GLOW; the accent for the stock look). The second is not drawn. */
        void setGlows(int color, int color2) {
            if (color != glow) {
                glow = color;
                shaderW = -1;
                invalidateSelf();
            }
        }

        /** The hero backdrop (null: none). */
        void setHero(android.graphics.Bitmap b) {
            if (b != hero) {
                hero = b;
                shaderW = -1;
                invalidateSelf();
            }
        }

        int glowForTest() {
            return glow;
        }

        android.graphics.Bitmap heroForTest() {
            return hero;
        }

        private void makeShaders(float w, float h) {
            bg = new LinearGradient(0, 0, 0, h, Theme.BG_TOP, Theme.BG_BOTTOM, Shader.TileMode.CLAMP);
            glowS = hero != null ? null
                    : new RadialGradient(w * 0.86f, h * 0.08f, w * 0.55f, Theme.alpha(glow, GLOW_ALPHA), 0,
                                         Shader.TileMode.CLAMP);
            heroS = null;
            if (hero != null) {
                // Cover-fit, centred; gone left of 30 % of the width, at HERO_ALPHA from 85 %; gone in the bottom 40 %,
                // where the buttons and hints stay on the plain gradient.
                // The hero is tiny (GamePalette.HERO_SIDE); HWUI samples a BitmapShader inside a ComposeShader
                // nearest-neighbour, which showed 40-pixel blocks. Upscale it smoothly first, and ask for linear
                // filtering where the platform has it.
                android.graphics.Bitmap img = hero;
                int up = Math.max(1, Math.round(Math.max(w, h) / 4 / Math.max(hero.getWidth(), hero.getHeight())));
                if (up > 1)
                    img = android.graphics.Bitmap.createScaledBitmap(hero, hero.getWidth() * up,
                                                                     hero.getHeight() * up, true);
                android.graphics.BitmapShader bs = new android.graphics.BitmapShader(img, Shader.TileMode.CLAMP,
                                                                                     Shader.TileMode.CLAMP);
                if (android.os.Build.VERSION.SDK_INT >= 33)
                    bs.setFilterMode(android.graphics.BitmapShader.FILTER_MODE_LINEAR);
                float sc = Math.max(w / img.getWidth(), h / img.getHeight());
                android.graphics.Matrix m = new android.graphics.Matrix();
                m.setScale(sc, sc);
                m.postTranslate((w - img.getWidth() * sc) / 2, (h - img.getHeight() * sc) / 2);
                bs.setLocalMatrix(m);
                int top = Theme.alpha(0xFF000000, HERO_ALPHA);
                Shader fx = new LinearGradient(w * 0.30f, 0, w * 0.85f, 0, 0x00000000, top, Shader.TileMode.CLAMP);
                Shader fy = new LinearGradient(0, h * 0.25f, 0, h * 0.60f, 0xFF000000, 0x00000000,
                                               Shader.TileMode.CLAMP);
                Shader mask = new android.graphics.ComposeShader(fx, fy, android.graphics.PorterDuff.Mode.DST_IN);
                heroS = new android.graphics.ComposeShader(bs, mask, android.graphics.PorterDuff.Mode.DST_IN);
            }
            shaderW = (int)w;
            shaderH = (int)h;
        }

        @Override
        public void draw(Canvas c) {
            Rect b = getBounds();
            float w = b.width(), h = b.height();
            if (shaderW != (int)w || shaderH != (int)h)
                makeShaders(w, h);
            paint.setShader(bg);
            c.drawRect(b, paint);
            if (heroS != null) {
                heroP.setShader(heroS);
                c.drawRect(b, heroP);
            }
            if (glowS != null) {
                paint.setShader(glowS);
                c.drawRect(b, paint);
            }
            paint.setShader(null);
        }
    }

    /** An overlay's surface (the Options sheet, the in-game drawer): a flat fill, no outline. */
    static final class Panel extends SimpleDrawable {
        private final Paint bg = fill(Theme.PANEL);
        private final RectF r = new RectF();
        private final float radius;

        Panel() {
            this(Theme.RADIUS);
        }

        Panel(float radius) {
            this.radius = radius;
        }

        @Override
        public void draw(Canvas c) {
            Rect b = getBounds();
            r.set(b);
            if (radius <= 0)
                c.drawRect(r, bg);
            else
                c.drawRoundRect(r, radius, radius, bg);
        }
    }

    // ---- Icons -----------------------------------------------------------------------------------------------------

    enum Icon {
        PLAY, SLIDERS, DISC, INFO, FOLDER, KEY, COPY, SWAP, TRASH, ARROW, CHECK, BACK, RESET, PAD, SOUND, BUZZ,
        // The library's search and sort pills, a favourite (XBA-206).
        SEARCH, SORT, STAR,
        // The in-game menu's (GameMenu), drawn by PanelIcons.
        CAMERA, SHARE, HOME, POWER, BARS, SCREEN
    }

    /** A five-pointed star of outer radius r centred on (cx, cy), point up (a favourite, XBA-206). */
    static Path star(float cx, float cy, float r) {
        Path p = new Path();
        for (int i = 0; i < 10; i++) {
            double a = Math.PI / 5 * i - Math.PI / 2;
            float rr = i % 2 == 0 ? r : r * 0.42f;
            float x = cx + (float)(Math.cos(a) * rr), y = cy + (float)(Math.sin(a) * rr);
            if (i == 0)
                p.moveTo(x, y);
            else
                p.lineTo(x, y);
        }
        p.close();
        return p;
    }

    /** Draws an icon in a box of side s centred on (cx, cy), with the stroke paint p (its colour is kept). */
    static void drawIcon(Canvas c, Icon icon, float cx, float cy, float s, Paint p) {
        float h = s / 2;
        Path path = new Path();
        Paint f = new Paint(p);
        f.setStyle(Paint.Style.FILL);
        switch (icon) {
        case PLAY:
            path.moveTo(cx - h * 0.45f, cy - h * 0.62f);
            path.lineTo(cx + h * 0.62f, cy);
            path.lineTo(cx - h * 0.45f, cy + h * 0.62f);
            path.close();
            c.drawPath(path, f);
            break;
        case SLIDERS:
            for (int i = -1; i <= 1; i++) {
                float y = cy + i * h * 0.6f, kx = cx + (i == 0 ? h * 0.35f : -h * 0.3f);
                c.drawLine(cx - h * 0.8f, y, cx + h * 0.8f, y, p);
                c.drawCircle(kx, y, h * 0.2f, f);
            }
            break;
        case DISC:
            c.drawCircle(cx, cy, h * 0.82f, p);
            c.drawCircle(cx, cy, h * 0.22f, p);
            c.drawArc(cx - h * 0.55f, cy - h * 0.55f, cx + h * 0.55f, cy + h * 0.55f, 200, 60, false, p);
            break;
        case SEARCH:
            c.drawCircle(cx - h * 0.15f, cy - h * 0.15f, h * 0.55f, p);
            c.drawLine(cx + h * 0.25f, cy + h * 0.25f, cx + h * 0.8f, cy + h * 0.8f, p);
            break;
        case SORT:
            for (int i = -1; i <= 1; i++) {   // three lines, shorter and shorter
                float y = cy + i * h * 0.55f;
                c.drawLine(cx - h * 0.8f, y, cx - h * 0.8f + h * (1.6f - (i + 1) * 0.45f), y, p);
            }
            break;
        case STAR:
            c.drawPath(star(cx, cy, h * 0.9f), f);
            break;
        case INFO:
            c.drawCircle(cx, cy, h * 0.82f, p);
            c.drawLine(cx, cy - h * 0.05f, cx, cy + h * 0.42f, p);
            c.drawCircle(cx, cy - h * 0.36f, p.getStrokeWidth() * 0.75f, f);
            break;
        case FOLDER:
            path.moveTo(cx - h * 0.85f, cy + h * 0.6f);
            path.lineTo(cx - h * 0.85f, cy - h * 0.6f);
            path.lineTo(cx - h * 0.25f, cy - h * 0.6f);
            path.lineTo(cx - h * 0.05f, cy - h * 0.38f);
            path.lineTo(cx + h * 0.85f, cy - h * 0.38f);
            path.lineTo(cx + h * 0.85f, cy + h * 0.6f);
            path.close();
            c.drawPath(path, p);
            break;
        case KEY:
            c.drawCircle(cx - h * 0.4f, cy, h * 0.36f, p);
            c.drawLine(cx - h * 0.04f, cy, cx + h * 0.85f, cy, p);
            c.drawLine(cx + h * 0.55f, cy, cx + h * 0.55f, cy + h * 0.32f, p);
            c.drawLine(cx + h * 0.82f, cy, cx + h * 0.82f, cy + h * 0.26f, p);
            break;
        case COPY:
            c.drawRoundRect(cx - h * 0.75f, cy - h * 0.45f, cx + h * 0.35f, cy + h * 0.8f, h * 0.12f, h * 0.12f, p);
            path.moveTo(cx - h * 0.35f, cy - h * 0.65f);
            path.lineTo(cx - h * 0.35f, cy - h * 0.8f);
            path.lineTo(cx + h * 0.75f, cy - h * 0.8f);
            path.lineTo(cx + h * 0.75f, cy + h * 0.4f);
            path.lineTo(cx + h * 0.55f, cy + h * 0.4f);
            c.drawPath(path, p);
            break;
        case SWAP:
            c.drawLine(cx - h * 0.75f, cy - h * 0.3f, cx + h * 0.75f, cy - h * 0.3f, p);
            c.drawLine(cx + h * 0.45f, cy - h * 0.6f, cx + h * 0.75f, cy - h * 0.3f, p);
            c.drawLine(cx + h * 0.75f, cy + h * 0.3f, cx - h * 0.75f, cy + h * 0.3f, p);
            c.drawLine(cx - h * 0.45f, cy + h * 0.6f, cx - h * 0.75f, cy + h * 0.3f, p);
            break;
        case TRASH:
            c.drawLine(cx - h * 0.8f, cy - h * 0.5f, cx + h * 0.8f, cy - h * 0.5f, p);
            c.drawLine(cx - h * 0.25f, cy - h * 0.75f, cx + h * 0.25f, cy - h * 0.75f, p);
            path.moveTo(cx - h * 0.6f, cy - h * 0.5f);
            path.lineTo(cx - h * 0.48f, cy + h * 0.8f);
            path.lineTo(cx + h * 0.48f, cy + h * 0.8f);
            path.lineTo(cx + h * 0.6f, cy - h * 0.5f);
            c.drawPath(path, p);
            break;
        case ARROW:
            c.drawLine(cx - h * 0.7f, cy, cx + h * 0.7f, cy, p);
            c.drawLine(cx + h * 0.25f, cy - h * 0.45f, cx + h * 0.7f, cy, p);
            c.drawLine(cx + h * 0.25f, cy + h * 0.45f, cx + h * 0.7f, cy, p);
            break;
        case CHECK:
            c.drawLine(cx - h * 0.6f, cy, cx - h * 0.15f, cy + h * 0.45f, p);
            c.drawLine(cx - h * 0.15f, cy + h * 0.45f, cx + h * 0.65f, cy - h * 0.45f, p);
            break;
        case BACK:
            c.drawLine(cx + h * 0.7f, cy, cx - h * 0.7f, cy, p);
            c.drawLine(cx - h * 0.25f, cy - h * 0.45f, cx - h * 0.7f, cy, p);
            c.drawLine(cx - h * 0.25f, cy + h * 0.45f, cx - h * 0.7f, cy, p);
            break;
        case RESET:
            c.drawArc(cx - h * 0.7f, cy - h * 0.7f, cx + h * 0.7f, cy + h * 0.7f, -60, 300, false, p);
            c.drawLine(cx + h * 0.35f, cy - h * 0.61f, cx + h * 0.35f, cy - h * 0.2f, p);
            c.drawLine(cx + h * 0.35f, cy - h * 0.61f, cx - h * 0.05f, cy - h * 0.61f, p);
            break;
        case PAD:
            c.drawRoundRect(cx - h * 0.85f, cy - h * 0.45f, cx + h * 0.85f, cy + h * 0.5f, h * 0.42f, h * 0.42f, p);
            c.drawLine(cx - h * 0.62f, cy, cx - h * 0.28f, cy, p);
            c.drawLine(cx - h * 0.45f, cy - h * 0.17f, cx - h * 0.45f, cy + h * 0.17f, p);
            c.drawCircle(cx + h * 0.36f, cy - h * 0.1f, p.getStrokeWidth() * 0.8f, f);
            c.drawCircle(cx + h * 0.56f, cy + h * 0.1f, p.getStrokeWidth() * 0.8f, f);
            break;
        case SOUND:
            path.moveTo(cx - h * 0.75f, cy - h * 0.25f);
            path.lineTo(cx - h * 0.4f, cy - h * 0.25f);
            path.lineTo(cx, cy - h * 0.62f);
            path.lineTo(cx, cy + h * 0.62f);
            path.lineTo(cx - h * 0.4f, cy + h * 0.25f);
            path.lineTo(cx - h * 0.75f, cy + h * 0.25f);
            path.close();
            c.drawPath(path, p);
            c.drawArc(cx - h * 0.35f, cy - h * 0.45f, cx + h * 0.45f, cy + h * 0.45f, -50, 100, false, p);
            c.drawArc(cx - h * 0.25f, cy - h * 0.8f, cx + h * 0.8f, cy + h * 0.8f, -50, 100, false, p);
            break;
        case BUZZ:
            c.drawRoundRect(cx - h * 0.3f, cy - h * 0.7f, cx + h * 0.3f, cy + h * 0.7f, h * 0.12f, h * 0.12f, p);
            c.drawLine(cx - h * 0.55f, cy - h * 0.3f, cx - h * 0.55f, cy + h * 0.3f, p);
            c.drawLine(cx + h * 0.55f, cy - h * 0.3f, cx + h * 0.55f, cy + h * 0.3f, p);
            c.drawLine(cx - h * 0.78f, cy - h * 0.15f, cx - h * 0.78f, cy + h * 0.15f, p);
            c.drawLine(cx + h * 0.78f, cy - h * 0.15f, cx + h * 0.78f, cy + h * 0.15f, p);
            break;
        case CAMERA:
            PanelIcons.draw(c, PanelIcons.Icon.CAMERA, cx, cy, s, p);
            break;
        case SHARE:
            PanelIcons.draw(c, PanelIcons.Icon.SHARE, cx, cy, s, p);
            break;
        case HOME:
            PanelIcons.draw(c, PanelIcons.Icon.HOME, cx, cy, s, p);
            break;
        case POWER:
            PanelIcons.draw(c, PanelIcons.Icon.POWER, cx, cy, s, p);
            break;
        case BARS:
            PanelIcons.draw(c, PanelIcons.Icon.BARS, cx, cy, s, p);
            break;
        case SCREEN:
            PanelIcons.draw(c, PanelIcons.Icon.SCREEN, cx, cy, s, p);
            break;
        }
    }

    /** The app's mark: a rounded square around a glowing diamond. Generic, drawn in code. */
    static final class BrandMark extends View {
        private final Paint frame = stroke(Theme.ACCENT, 3.5f);
        private final Paint gem = fill(Theme.ACCENT);
        private final RectF r = new RectF();
        private final Path p = new Path();

        BrandMark(Context c) {
            super(c);
            gem.setShadowLayer(14, 0, 0, Theme.alpha(Theme.ACCENT, 0.7f));
        }

        @Override
        protected void onDraw(Canvas c) {
            float s = Math.min(getWidth(), getHeight()), cx = getWidth() / 2f, cy = getHeight() / 2f;
            r.set(cx - s * 0.42f, cy - s * 0.42f, cx + s * 0.42f, cy + s * 0.42f);
            c.drawRoundRect(r, s * 0.16f, s * 0.16f, frame);
            float d = s * 0.2f;
            p.reset();
            p.moveTo(cx, cy - d);
            p.lineTo(cx + d, cy);
            p.lineTo(cx, cy + d);
            p.lineTo(cx - d, cy);
            p.close();
            c.drawPath(p, gem);
        }
    }

    // ---- Selectable: what the focus lands on (list rows, buttons) ----------------------------------------------------

    interface Cycler {
        void step(int dir);
    }

    /**
     * A focusable the launcher's one highlight (Glide) lands on: a list row (MenuRow) or a button (PillButton). It
     * grows a little when focused (Theme.FOCUS_SCALE) and runs onPress (the « select » feedback) before a click.
     * Dim ones stay focusable so that a press can say why nothing happens.
     */
    abstract static class Selectable extends View {
        String title = "";
        boolean dim;
        /** Run on a click (A, Enter or a tap) before the click listener: the launcher's « select » feedback. */
        Runnable onPress;
        float focusT;
        private ValueAnimator anim;

        Selectable(Context c) {
            super(c);
            setFocusable(true);
            setClickable(true);
            // The plate is drawn here; Android's own focus highlight (API 26+) would add a grey square behind it.
            setDefaultFocusHighlightEnabled(false);
        }

        /** The focus plate inside the view: its inset on the sides and at the top and bottom, and its corner. */
        abstract float plateInsetX();

        abstract float plateInsetY();

        abstract float plateRadius(float plateHeight);

        void setDim(boolean dim) {
            this.dim = dim;
            invalidate();
        }

        void setTitle(String s) {
            title = s == null ? "" : s;
            invalidate();
        }

        ValueAnimator focusAnimatorForTest() {
            return anim;
        }

        @Override
        public boolean performClick() {
            if (onPress != null)
                onPress.run();
            return super.performClick();
        }

        @Override
        protected void onFocusChanged(boolean gained, int direction, Rect previous) {
            super.onFocusChanged(gained, direction, previous);
            if (anim != null)
                anim.cancel();
            anim = ValueAnimator.ofFloat(focusT, gained ? 1 : 0);
            anim.setDuration(Theme.ANIM_FOCUS_MS);
            anim.addUpdateListener(a -> {
                focusT = (float)a.getAnimatedValue();
                float s = 1 + (Theme.FOCUS_SCALE - 1) * focusT;
                setScaleX(s);
                setScaleY(s);
                invalidate();
            });
            anim.start();
        }

        @Override
        protected void drawableStateChanged() {
            super.drawableStateChanged();
            invalidate();
        }
    }

    // ---- MenuRow: a list row (label and value) ------------------------------------------------------------------------

    /**
     * A list row, drawn on Canvas: its label, and on the right a value or a badge (« Press A again »). A setting
     * (cycler set) shows « < value > » arrows when focused only; left and right step it (and so do taps on the
     * arrows), and the value pulses when it changes. A setting keeps the width of its widest choice (setChoices), so the
     * label does not move as the value changes; when the label and that width do not fit side by side (a narrow
     * column: the in-game menu's drawer, « As in Settings (…) » choices), the row is stacked: the label on the first
     * line, whole, the value under it, the arrows at the right edge. The row's description is not drawn: the screen shows the
     * focused row's in a quiet column of its own. A subtitle (one line under the label) is for the rare row that must
     * say more where it is (the in-game menu's « Press again to quit »).
     */
    static final class MenuRow extends Selectable {
        /** What the focused row's description column says. */
        String description;
        /** Drawn under the label, one line (rare: see the class comment). */
        String subtitle;
        String badge, value;
        int badgeColor = Theme.TEXT_DIM;
        Cycler cycler;
        /**
         * XBA-182: a picture at the right side (a save state's), as tall as the focus plate, its aspect kept; with one
         * the value and the badge are not drawn.
         */
        android.graphics.Bitmap thumb;

        private float tickT, valueReserve;
        private ValueAnimator tickAnim;
        private final Paint fillP = fill(0), chevP = fill(Theme.ACCENT);
        private final TextPaint titleP = textPaint(Theme.T_ROW, Theme.MEDIUM, Theme.TEXT);
        private final TextPaint subP = textPaint(Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM);
        private final TextPaint valueP = textPaint(Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM);
        private final TextPaint badgeP = textPaint(Theme.T_SMALL, Theme.MEDIUM, Theme.TEXT_DIM);
        private final RectF r = new RectF(), thumbR = new RectF();
        private final Path tri = new Path(), thumbClip = new Path();
        private final Paint thumbP = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private float leftArrowX = -1;
        private boolean downOnLeftArrow;
        private static final float ARROW = 10, ARROW_GAP = 18, THUMB_PAD = 4, THUMB_RADIUS = 8;

        MenuRow(Context c) {
            super(c);
        }

        MenuRow set(String title, String description) {
            this.title = title == null ? "" : title;
            this.description = description;
            invalidate();
            return this;
        }

        @Override
        float plateInsetX() {
            return 0;
        }

        @Override
        float plateInsetY() {
            return 4;
        }

        @Override
        float plateRadius(float plateHeight) {
            return Theme.ROW_RADIUS;
        }

        /** The labels a setting can show: the value column is as wide as the widest. */
        void setChoices(String[] labels) {
            float m = 0;
            for (String l : labels)
                m = Math.max(m, valueP.measureText(l));
            valueReserve = m;
            invalidate();
        }

        void setValue(String value) {
            boolean changed = this.value != null && !this.value.equals(value);
            this.value = value;
            if (changed)
                tick();
            invalidate();
        }

        /** The value's pulse (Theme.ANIM_TICK_MS). */
        private void tick() {
            if (tickAnim != null)
                tickAnim.cancel();
            tickAnim = ValueAnimator.ofFloat(1, 0);
            tickAnim.setDuration(Theme.ANIM_TICK_MS);
            tickAnim.addUpdateListener(a -> {
                tickT = (float)a.getAnimatedValue();
                invalidate();
            });
            tickAnim.start();
        }

        ValueAnimator tickAnimatorForTest() {
            return tickAnim;
        }

        void setBadge(String badge, int color) {
            this.badge = badge;
            this.badgeColor = color;
            invalidate();
        }

        void setDescription(String s) {
            description = s;
        }

        void setSubtitle(String s) {
            subtitle = s;
            invalidate();
        }

        void setThumb(android.graphics.Bitmap b) {
            thumb = b;
            invalidate();
        }

        @Override
        public boolean onKeyDown(int keyCode, KeyEvent e) {
            if (cycler != null && (keyCode == KeyEvent.KEYCODE_DPAD_LEFT || keyCode == KeyEvent.KEYCODE_DPAD_RIGHT)) {
                cycler.step(keyCode == KeyEvent.KEYCODE_DPAD_LEFT ? -1 : 1);
                return true;
            }
            return super.onKeyDown(keyCode, e);
        }

        /** A tap that starts and ends on the left arrow steps back; any other tap is a click (one step forward). */
        @Override
        public boolean onTouchEvent(MotionEvent e) {
            if (e.getAction() == MotionEvent.ACTION_DOWN)
                downOnLeftArrow = cycler != null && leftArrowX >= 0 && Math.abs(e.getX() - leftArrowX) < 48;
            if (downOnLeftArrow && e.getAction() == MotionEvent.ACTION_UP && Math.abs(e.getX() - leftArrowX) < 48) {
                downOnLeftArrow = false;
                MotionEvent cancel = MotionEvent.obtain(e);
                cancel.setAction(MotionEvent.ACTION_CANCEL);
                super.onTouchEvent(cancel);   // clears the pressed state without a click
                cancel.recycle();
                cycler.step(-1);
                return true;
            }
            return super.onTouchEvent(e);
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(), t = focusT;
            r.set(plateInsetX(), plateInsetY(), w - plateInsetX(), h - plateInsetY());
            if (isPressed()) {
                fillP.setColor(Theme.alpha(Theme.ACCENT, 0.12f));
                c.drawRoundRect(r, Theme.ROW_RADIUS, Theme.ROW_RADIUS, fillP);
            }
            float x = Theme.ROW_INSET, right = w - Theme.ROW_INSET, textRight = right;
            leftArrowX = -1;
            if (thumb != null && thumb.getWidth() > 0 && thumb.getHeight() > 0) {
                float th = Math.max(1, h - 2 * plateInsetY() - 2 * THUMB_PAD);
                float tw = th * thumb.getWidth() / thumb.getHeight();
                thumbR.set(right - tw, (h - th) / 2, right, (h + th) / 2);
                thumbClip.reset();
                thumbClip.addRoundRect(thumbR, THUMB_RADIUS, THUMB_RADIUS, Path.Direction.CW);
                c.save();
                c.clipPath(thumbClip);
                thumbP.setAlpha(dim ? 110 : 255);
                c.drawBitmap(thumb, null, thumbR, thumbP);
                c.restore();
                textRight = right - tw - Theme.S4;
            } else if (value != null && stacked(x, right)) {
                drawStacked(c, x, right, h, t);
                return;
            } else if (value != null && (badge == null || badge.isEmpty() || cycler != null)) {
                float vw = valueP.measureText(value), col = Math.max(vw, valueReserve);
                float arrows = cycler != null ? ARROW + ARROW_GAP : 0;
                // The value sits at the right edge, and moves left to make room for its arrows when focused.
                float vRight = right - arrows * t, vx = vRight - vw, cy = h / 2;
                valueP.setColor(dim ? Theme.TEXT_FAINT
                                    : blend(blend(Theme.TEXT_DIM, Theme.ACCENT, t), Theme.ACCENT, tickT));
                Paint.FontMetrics fm = valueP.getFontMetrics();
                c.save();
                float s = 1 + 0.14f * tickT;
                c.scale(s, s, vx + vw / 2, cy);
                c.drawText(value, vx, cy - (fm.ascent + fm.descent) / 2, valueP);
                c.restore();
                if (cycler != null && t > 0.01f && !dim) {
                    chevP.setColor(Theme.alpha(Theme.ACCENT, t));
                    float lx = vx - ARROW_GAP - ARROW;
                    triangle(c, lx, cy, ARROW, -1);
                    triangle(c, right - ARROW, cy, ARROW, 1);
                    leftArrowX = lx + ARROW / 2;
                } else if (cycler != null) {
                    leftArrowX = vx - ARROW_GAP - ARROW / 2;
                }
                textRight = right - 2 * arrows - col - Theme.S4;
                if (badge != null && !badge.isEmpty()) {   // a setting's badge (« This game »): left of its value
                    float bw = badgeP.measureText(badge), bx = right - 2 * arrows - col - Theme.S2 - bw;
                    badgeP.setColor(dim ? Theme.TEXT_FAINT : badgeColor);
                    Paint.FontMetrics bf = badgeP.getFontMetrics();
                    c.drawText(badge, bx, cy - (bf.ascent + bf.descent) / 2, badgeP);
                    textRight = bx - Theme.S4;
                }
            } else if (badge != null && !badge.isEmpty()) {
                badgeP.setColor(badgeColor);
                float bw = badgeP.measureText(badge);
                Paint.FontMetrics fm = badgeP.getFontMetrics();
                c.drawText(badge, right - bw, h / 2 - (fm.ascent + fm.descent) / 2, badgeP);
                textRight = right - bw - Theme.S4;
            }

            float avail = Math.max(10, textRight - x);
            titleP.setColor(dim ? Theme.TEXT_FAINT : blend(Theme.TEXT, Theme.TEXT_BRIGHT, t));
            String ti = fit(title, titleP, avail);
            Paint.FontMetrics tf = titleP.getFontMetrics();
            if (subtitle == null || subtitle.isEmpty()) {
                c.drawText(ti, x, h / 2 - (tf.ascent + tf.descent) / 2, titleP);
                return;
            }
            Paint.FontMetrics sf = subP.getFontMetrics();
            float titleH = -tf.ascent, subH = -sf.ascent, gap = Theme.S1;
            float top = (h - (titleH + gap + subH)) / 2;
            c.drawText(ti, x, top + titleH, titleP);
            subP.setColor(dim ? Theme.TEXT_FAINT : Theme.TEXT_DIM);
            c.drawText(fit(subtitle, subP, avail), x, top + titleH + gap + subH, subP);
        }

        /**
         * A setting whose label and widest value do not fit side by side (the label would be cut): stacked. Decided on
         * the widest choice, not the value shown, so a row does not switch layout as it is stepped. A setting's badge
         * (« This game ») counts too: it sits left of the value, or after it under the label when stacked.
         */
        private boolean stacked(float x, float right) {
            if (cycler == null || (subtitle != null && !subtitle.isEmpty()))
                return false;
            float col = Math.max(valueP.measureText(value), valueReserve);
            float tag = badge != null && !badge.isEmpty() ? badgeP.measureText(badge) + Theme.S2 : 0;
            return titleP.measureText(title) > right - 2 * (ARROW + ARROW_GAP) - col - tag - Theme.S4 - x;
        }

        boolean stackedForTest() {
            return value != null && stacked(Theme.ROW_INSET, getWidth() - Theme.ROW_INSET);
        }

        /**
         * The stacked row: the label, whole, then the value (and its badge) under it; the arrows at the right edge when
         * focused.
         */
        private void drawStacked(Canvas c, float x, float right, float h, float t) {
            float arrows = 2 * (ARROW + ARROW_GAP);
            float avail = Math.max(10, right - arrows - x);
            Paint.FontMetrics tf = titleP.getFontMetrics(), vf = valueP.getFontMetrics();
            float titleH = -tf.ascent, valueH = -vf.ascent, gap = Theme.S1 / 2f;
            float top = (h - (titleH + gap + valueH + vf.descent)) / 2;
            titleP.setColor(dim ? Theme.TEXT_FAINT : blend(Theme.TEXT, Theme.TEXT_BRIGHT, t));
            c.drawText(fit(title, titleP, avail), x, top + titleH, titleP);
            String v = fit(value, valueP, avail);
            float vw = valueP.measureText(v), vy = top + titleH + gap + valueH;
            valueP.setColor(dim ? Theme.TEXT_FAINT : blend(blend(Theme.TEXT_DIM, Theme.ACCENT, t), Theme.ACCENT, tickT));
            c.save();
            float s = 1 + 0.14f * tickT;
            c.scale(s, s, x + vw / 2, vy + (vf.ascent + vf.descent) / 2);
            c.drawText(v, x, vy, valueP);
            c.restore();
            if (badge != null && !badge.isEmpty()) {
                float bx = x + vw + Theme.S2;
                badgeP.setColor(dim ? Theme.TEXT_FAINT : badgeColor);
                c.drawText(fit(badge, badgeP, Math.max(0, x + avail - bx)), bx, vy, badgeP);
            }
            float cy = h / 2, lx = right - ARROW - ARROW_GAP - ARROW;
            if (t > 0.01f && !dim) {
                chevP.setColor(Theme.alpha(Theme.ACCENT, t));
                triangle(c, lx, cy, ARROW, -1);
                triangle(c, right - ARROW, cy, ARROW, 1);
            }
            leftArrowX = lx + ARROW / 2;
        }

        private void triangle(Canvas c, float x, float cy, float s, int dir) {
            tri.reset();
            if (dir < 0) {
                tri.moveTo(x + s, cy - s);
                tri.lineTo(x, cy);
                tri.lineTo(x + s, cy + s);
            } else {
                tri.moveTo(x, cy - s);
                tri.lineTo(x + s, cy);
                tri.lineTo(x, cy + s);
            }
            tri.close();
            c.drawPath(tri, chevP);
        }
    }

    // ---- PillButton: Home's Play and its quiet neighbours ---------------------------------------------------------

    /**
     * A pill-shaped button with an optional icon. The primary one (Play, Prepare) is solid ACCENT with GLYPH_INK text
     * when focused, ACCENT on ACCENT_FACE otherwise; a quiet one (Settings, Controls, Manage) is a soft PANEL_HI pill
     * that the focus plate lights. A dim primary stays quiet.
     */
    static final class PillButton extends Selectable {
        Icon icon;
        private boolean primary;
        /** Icon only, a circle (PillRow, when the labels do not fit on one line). */
        private boolean compact;
        private final Paint fillP = fill(0), iconP = stroke(Theme.TEXT, 3.5f);
        private final TextPaint labelP = textPaint(Theme.T_ROW, Theme.MEDIUM, Theme.TEXT);
        private final RectF r = new RectF();

        PillButton(Context c) {
            super(c);
        }

        PillButton set(Icon icon, String title, boolean primary) {
            this.icon = icon;
            this.title = title == null ? "" : title;
            this.primary = primary;
            requestLayout();
            invalidate();
            return this;
        }

        boolean primaryForTest() {
            return primary;
        }

        boolean canCompact() {
            return icon != null && !primary;
        }

        boolean isCompact() {
            return compact;
        }

        /** Icon only (the label becomes the content description), or back to icon and label. */
        void setCompact(boolean on) {
            on = on && canCompact();
            if (on == compact)
                return;
            compact = on;
            setContentDescription(on ? title : null);
            requestLayout();
            invalidate();
        }

        /** The width the label and icon need, padding included. */
        int naturalWidth() {
            if (compact)
                return Theme.BUTTON_H;
            float w = labelP.measureText(title) + 2 * Theme.S5;
            if (icon != null)
                w += 28 + Theme.S2;
            return Math.round(w);
        }

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getMode(wSpec) == MeasureSpec.EXACTLY ? MeasureSpec.getSize(wSpec) : naturalWidth();
            int h = MeasureSpec.getMode(hSpec) == MeasureSpec.EXACTLY ? MeasureSpec.getSize(hSpec) : Theme.BUTTON_H;
            setMeasuredDimension(w, h);
        }

        @Override
        float plateInsetX() {
            return 0;
        }

        @Override
        float plateInsetY() {
            return 0;
        }

        @Override
        float plateRadius(float plateHeight) {
            return plateHeight / 2;
        }

        /** The fill when focused (tests: the accent of the theme). */
        int focusedFillForTest() {
            return primary && !dim ? Theme.ACCENT : Theme.PANEL_HI;
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(), t = focusT;
            r.set(0, 0, w, h);
            int face, ink;
            if (primary && !dim) {
                face = blend(Theme.ACCENT_FACE, Theme.ACCENT, t);
                ink = blend(Theme.ACCENT, Theme.GLYPH_INK, t);
            } else {
                face = Theme.alpha(Theme.PANEL_HI, 0.8f);
                ink = dim ? Theme.TEXT_FAINT : blend(Theme.TEXT, Theme.TEXT_BRIGHT, t);
            }
            fillP.setColor(face);
            c.drawRoundRect(r, h / 2, h / 2, fillP);
            if (isPressed()) {
                fillP.setColor(Theme.alpha(Theme.ACCENT, 0.12f));
                c.drawRoundRect(r, h / 2, h / 2, fillP);
            }
            labelP.setColor(ink);
            iconP.setColor(ink);
            float iw = icon != null ? 28 : 0, ig = icon != null ? Theme.S2 : 0;
            if (compact) {
                drawIcon(c, icon, w / 2, h / 2, iw, iconP);
                return;
            }
            String label = fit(title, labelP, Math.max(10, w - 2 * Theme.S4 - iw - ig));
            float lw = labelP.measureText(label), total = iw + ig + lw, x = (w - total) / 2;
            if (icon != null)
                drawIcon(c, icon, x + iw / 2, h / 2, iw, iconP);
            Paint.FontMetrics fm = labelP.getFontMetrics();
            c.drawText(label, x + iw + ig, h / 2 - (fm.ascent + fm.descent) / 2, labelP);
        }
    }

    /**
     * A row of PillButtons that stays on one line: when the buttons do not fit, the quiet pills with an icon turn
     * into icon-only circles, from the last one back, until they do.
     */
    static final class PillRow extends LinearLayout {
        PillRow(Context c) {
            super(c);
            setOrientation(HORIZONTAL);
        }

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            for (int i = 0; i < getChildCount(); i++)
                if (getChildAt(i) instanceof PillButton)
                    ((PillButton)getChildAt(i)).setCompact(false);
            super.onMeasure(wSpec, hSpec);
            if (MeasureSpec.getMode(wSpec) == MeasureSpec.UNSPECIFIED)
                return;
            int avail = MeasureSpec.getSize(wSpec) - getPaddingLeft() - getPaddingRight();
            for (int i = getChildCount() - 1; i >= 0 && used() > avail; i--) {
                View v = getChildAt(i);
                if (v instanceof PillButton && ((PillButton)v).canCompact()) {
                    ((PillButton)v).setCompact(true);
                    super.onMeasure(wSpec, hSpec);
                }
            }
        }

        /** The children's measured widths and margins. */
        private int used() {
            int u = 0;
            for (int i = 0; i < getChildCount(); i++) {
                View v = getChildAt(i);
                if (v.getVisibility() == GONE)
                    continue;
                LayoutParams p = (LayoutParams)v.getLayoutParams();
                u += v.getMeasuredWidth() + p.leftMargin + p.rightMargin;
            }
            return u;
        }
    }

    // ---- Tabs: a screen's sections, switched with L1 / R1 -------------------------------------------------------------

    /**
     * The tabs of a screen's top bar (Settings): the labels, the active one in TEXT with an ACCENT underline, the
     * others TEXT_DIM, flanked by the L1 and R1 glyphs that switch them. Not focusable; a tap on a label opens it, on a
     * glyph the previous or next one.
     */
    static final class Tabs extends View {
        interface Listener {
            void onTab(int index);
        }

        private final String[] labels;
        private final int active;
        private final Listener listener;
        private final TextPaint tp = textPaint(Theme.T_ROW, Theme.REGULAR, Theme.TEXT_DIM);
        private final TextPaint gp = textPaint(Theme.T_SMALL - 2, Theme.MEDIUM, Theme.TEXT_DIM);
        private final Paint ringP = stroke(Theme.TEXT_DIM, 2), lineP = fill(Theme.ACCENT);
        private final float[] left, width;
        private final RectF r = new RectF();
        private static final float GLYPH_W = 48, GLYPH_H = 40, GAP = Theme.S5, UNDERLINE = 4;

        Tabs(Context c, String[] labels, int active, Listener listener) {
            super(c);
            this.labels = labels;
            this.active = active;
            this.listener = listener;
            left = new float[labels.length];
            width = new float[labels.length];
            float x = GLYPH_W + GAP - Theme.S2;
            for (int i = 0; i < labels.length; i++) {
                width[i] = tp.measureText(labels[i]);
                left[i] = x;
                x += width[i] + GAP;
            }
            setFocusable(false);
        }

        int activeForTest() {
            return active;
        }

        /** The underline's colour: the accent the screen was built with. */
        int accentForTest() {
            return lineP.getColor();
        }

        private float total() {
            int n = labels.length;
            return n == 0 ? 2 * GLYPH_W : left[n - 1] + width[n - 1] + GAP - Theme.S2 + GLYPH_W;
        }

        @Override
        protected void onMeasure(int w, int h) {
            setMeasuredDimension(Math.round(total()) + 2, MeasureSpec.getMode(h) == MeasureSpec.EXACTLY
                                                          ? MeasureSpec.getSize(h) : Theme.TOP_BAR_H);
        }

        private void glyph(Canvas c, float x, float cy, String g) {
            r.set(x + 1, cy - GLYPH_H / 2, x + GLYPH_W - 1, cy + GLYPH_H / 2);
            c.drawRoundRect(r, GLYPH_H / 2, GLYPH_H / 2, ringP);
            Paint.FontMetrics fm = gp.getFontMetrics();
            c.drawText(g, x + GLYPH_W / 2 - gp.measureText(g) / 2, cy - (fm.ascent + fm.descent) / 2, gp);
        }

        @Override
        protected void onDraw(Canvas c) {
            float cy = getHeight() / 2f;
            glyph(c, 0, cy, "L1");
            Paint.FontMetrics fm = tp.getFontMetrics();
            float base = cy - (fm.ascent + fm.descent) / 2;
            for (int i = 0; i < labels.length; i++) {
                tp.setColor(i == active ? Theme.TEXT : Theme.TEXT_DIM);
                c.drawText(labels[i], left[i], base, tp);
                if (i == active)
                    c.drawRoundRect(left[i], base + Theme.S2, left[i] + width[i], base + Theme.S2 + UNDERLINE,
                                    UNDERLINE / 2, UNDERLINE / 2, lineP);
            }
            glyph(c, total() - GLYPH_W, cy, "R1");
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            if (e.getAction() == MotionEvent.ACTION_DOWN)
                return true;
            if (e.getAction() != MotionEvent.ACTION_UP || listener == null)
                return super.onTouchEvent(e);
            float x = e.getX();
            int n = labels.length;
            if (x < GLYPH_W) {
                listener.onTab((active - 1 + n) % n);
                return true;
            }
            if (x > total() - GLYPH_W) {
                listener.onTab((active + 1) % n);
                return true;
            }
            for (int i = 0; i < n; i++) {
                if (x >= left[i] - GAP / 2 && x <= left[i] + width[i] + GAP / 2) {
                    if (i != active)
                        listener.onTab(i);
                    return true;
                }
            }
            return true;
        }
    }

    // ---- Sheet: a list over the screen, from the right ------------------------------------------------------------

    /**
     * An overlay list (the library's Options): a scrim over the screen, and on the right a flat PANEL sheet with a
     * title, MenuRows and its own hints. The launcher adds the rows, keeps the focus in it while it is open, and
     * closes it on B (or a tap on the scrim: onClose).
     */
    static final class Sheet extends android.widget.FrameLayout {
        static final int W = 600;
        static final float SCRIM = 0.55f;
        final android.widget.LinearLayout rows, hints;

        Sheet(Context c, String title, Runnable onClose) {
            super(c);
            setBackgroundColor(Theme.alpha(0xFF000000, SCRIM));
            setClickable(true);
            setOnClickListener(v -> onClose.run());
            android.widget.LinearLayout panel = new android.widget.LinearLayout(c);
            panel.setOrientation(android.widget.LinearLayout.VERTICAL);
            panel.setBackground(new Panel(0));
            panel.setClickable(true);   // a tap on the sheet is not one on the scrim
            panel.setPadding(Theme.S5, Theme.MARGIN_TOP, Theme.S5, Theme.MARGIN_BOTTOM);
            android.widget.TextView t = Theme.oneLine(Theme.text(c, Theme.T_TITLE, Theme.MEDIUM, Theme.TEXT, title));
            t.setGravity(android.view.Gravity.CENTER_VERTICAL);
            android.widget.LinearLayout.LayoutParams tl =
                    new android.widget.LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, Theme.TOP_BAR_H);
            tl.bottomMargin = Theme.S4;
            panel.addView(t, tl);
            rows = new android.widget.LinearLayout(c);
            rows.setOrientation(android.widget.LinearLayout.VERTICAL);
            rows.setClipChildren(false);
            android.widget.LinearLayout.LayoutParams rl =
                    new android.widget.LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, 0, 1);
            rl.leftMargin = rl.rightMargin = Theme.S3 - Theme.S5;   // the plates 24 px from the sheet's edges
            panel.addView(rows, rl);
            hints = new android.widget.LinearLayout(c);
            hints.setOrientation(android.widget.LinearLayout.HORIZONTAL);
            hints.setGravity(android.view.Gravity.CENTER_VERTICAL);
            panel.addView(hints, new android.widget.LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT,
                                                                              Theme.FOOTER_H));
            addView(panel, new LayoutParams(W, LayoutParams.MATCH_PARENT, android.view.Gravity.END));
        }

        void addRow(MenuRow r) {
            rows.addView(r, new android.widget.LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, Theme.ROW_H));
        }

        void addHint(Hint h) {
            android.widget.LinearLayout.LayoutParams p = new android.widget.LinearLayout.LayoutParams(
                    LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
            p.leftMargin = hints.getChildCount() == 0 ? 0 : Theme.S4;
            hints.addView(h, p);
        }

        java.util.List<MenuRow> rowsForTest() {
            java.util.List<MenuRow> out = new java.util.ArrayList<>();
            for (int i = 0; i < rows.getChildCount(); i++)
                out.add((MenuRow)rows.getChildAt(i));
            return out;
        }
    }

    // ---- Glide: the one focus highlight, gliding from row to row --------------------------------------------------

    /**
     * Drawn over the stage: a flat plate (ACCENT at 14 %) with a 2.5 px ACCENT outline around the focused row or
     * button, or a 4 px ACCENT ring around a focused game card's art. When the focus moves it glides from the old
     * place to the new one (Theme.ANIM_GLIDE_MS); when the target itself moves (a scroll, its focus scale) it follows
     * at once. Not focusable, lets touches through.
     */
    static final class Glide extends View implements ViewTreeObserver.OnPreDrawListener {
        /** The plate's opacity, the outline's and the ring's widths, the ring's gap to the art. */
        static final float PLATE_ALPHA = 0.14f, OUTLINE = 2.5f, RING = 4, RING_GAP = 4;
        private View target;
        private final RectF from = new RectF(), to = new RectF(), cur = new RectF(), tmp = new RectF();
        private final RectF art = new RectF();
        private float t = 1, fromRad, toRad;
        private ValueAnimator anim;
        private final Paint plateP = fill(Theme.alpha(Theme.ACCENT, PLATE_ALPHA)), lineP = stroke(Theme.ACCENT, OUTLINE);
        private final Paint ringP = stroke(Theme.ACCENT, RING);

        Glide(Context c) {
            super(c);
            setFocusable(false);
            setClickable(false);
        }

        ValueAnimator animatorForTest() {
            return anim;
        }

        View targetForTest() {
            return target;
        }

        /** The plate's colour (tests: the focus contrast). */
        int plateColorForTest() {
            return plateP.getColor();
        }

        /** The outline's colour: the accent the stage was built with (tests: the game theme reaches every view). */
        int outlineColorForTest() {
            return lineP.getColor();
        }

        @Override
        protected void onAttachedToWindow() {
            super.onAttachedToWindow();
            getViewTreeObserver().addOnPreDrawListener(this);
        }

        @Override
        protected void onDetachedFromWindow() {
            getViewTreeObserver().removeOnPreDrawListener(this);
            super.onDetachedFromWindow();
        }

        /** v's drawn bounds (its scale and every scroll on the way included) in the coordinates of host. */
        private boolean rectIn(View v, View host, RectF out) {
            out.set(0, 0, v.getWidth(), v.getHeight());
            View cur = v;
            while (cur != host) {
                cur.getMatrix().mapRect(out);
                out.offset(cur.getLeft(), cur.getTop());
                if (!(cur.getParent() instanceof View))
                    return false;
                View p = (View)cur.getParent();
                out.offset(-p.getScrollX(), -p.getScrollY());
                cur = p;
            }
            return true;
        }

        /** The highlight's place for f, from its drawn bounds b: the plate, or the ring around a card's art. */
        private float place(View f, RectF b, RectF out) {
            float k = f.getWidth() > 0 ? b.width() / f.getWidth() : 1;
            if (f instanceof GameCard) {
                ((GameCard)f).artRect(art);
                float g = (RING_GAP + RING / 2) * k;
                out.set(b.left + art.left * k - g, b.top + art.top * k - g, b.left + art.right * k + g,
                        b.top + art.bottom * k + g);
                return (Theme.ART_RADIUS + RING_GAP + RING / 2) * k;
            }
            Selectable s = (Selectable)f;
            out.set(b.left + s.plateInsetX() * k, b.top + s.plateInsetY() * k, b.right - s.plateInsetX() * k,
                    b.bottom - s.plateInsetY() * k);
            return s.plateRadius(out.height() / k) * k;
        }

        /** The highlight's rectangle now: from the old place to the new one, t of the way. */
        private void lerp(RectF out) {
            out.set(from.left + (to.left - from.left) * t, from.top + (to.top - from.top) * t,
                    from.right + (to.right - from.right) * t, from.bottom + (to.bottom - from.bottom) * t);
        }

        @Override
        public boolean onPreDraw() {
            View host = (View)getParent();
            View f = host == null ? null : host.findFocus();
            if (!(f instanceof Selectable || f instanceof GameCard) || !f.isShown() || !rectIn(f, host, tmp)) {
                if (target != null) {
                    target = null;
                    invalidate();
                }
                return true;
            }
            float rad = place(f, tmp, tmp);
            if (f != target) {
                if (target == null) {
                    from.set(tmp);
                    to.set(tmp);
                    fromRad = toRad = rad;
                    t = 1;
                } else {
                    lerp(cur);
                    fromRad = fromRad + (toRad - fromRad) * t;
                    from.set(cur);
                    to.set(tmp);
                    toRad = rad;
                    t = 0;
                    if (anim != null)
                        anim.cancel();
                    anim = ValueAnimator.ofFloat(0, 1);
                    anim.setDuration(Theme.ANIM_GLIDE_MS);
                    anim.setInterpolator(new android.view.animation.DecelerateInterpolator(1.6f));
                    anim.addUpdateListener(a -> {
                        t = (float)a.getAnimatedValue();
                        invalidate();
                    });
                    anim.start();
                }
                target = f;
                invalidate();
            } else if (!tmp.equals(to)) {
                if (anim == null || !anim.isRunning()) {
                    from.set(tmp);
                    fromRad = rad;
                }
                to.set(tmp);
                toRad = rad;
                invalidate();
            }
            return true;
        }

        @Override
        protected void onDraw(Canvas c) {
            if (target == null)
                return;
            lerp(cur);
            float rad = fromRad + (toRad - fromRad) * t;
            if (target instanceof GameCard) {
                c.drawRoundRect(cur, rad, rad, ringP);
                return;
            }
            if (target instanceof PillButton && ((PillButton)target).primary && !((PillButton)target).dim)
                return;   // the solid Play is its own highlight
            c.drawRoundRect(cur, rad, rad, plateP);
            RectF o = new RectF(cur);
            o.inset(OUTLINE / 2, OUTLINE / 2);
            c.drawRoundRect(o, rad, rad, lineP);
        }
    }

    // ---- ArtTile: the game's title image, or a monogram -------------------------------------------------------------

    /**
     * The game's identity on Home: its title image from the XBE (TitleArt) in a rounded tile, or, without one, a monogram (the title's initials on a gradient whose hue comes from the title ID), or,
     * with no game at all, a disc.
     */
    static final class ArtTile extends View {
        private android.graphics.Bitmap art;
        private String initials = "";
        private int color = Theme.ACCENT_DEEP;
        private boolean empty = true;
        private final Paint bmpP = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private final Paint bgP = fill(0xFF000000), lineP = stroke(Theme.alpha(Theme.TEXT, 0.10f), 2);
        private final Paint ringP = stroke(Theme.alpha(Theme.TEXT, 0.06f), 2);
        private final Paint iconP = stroke(Theme.TEXT_FAINT, 5);
        private final TextPaint monoP = textPaint(150, Theme.MEDIUM, Theme.alpha(Theme.TEXT_BRIGHT, 0.94f));
        private final RectF r = new RectF();
        private final Path clip = new Path();

        ArtTile(Context c) {
            super(c);
            monoP.setLetterSpacing(0.02f);
        }

        void setArt(android.graphics.Bitmap b, int color) {
            art = b;
            this.color = color;
            empty = false;
            invalidate();
        }

        void setMonogram(String initials, int color) {
            art = null;
            this.initials = initials == null ? "" : initials;
            this.color = color;
            empty = false;
            invalidate();
        }

        void setEmpty() {
            art = null;
            empty = true;
            color = Theme.LINE;
            invalidate();
        }

        boolean hasArtForTest() {
            return art != null;
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(), in = 1, rad = Theme.ART_RADIUS;
            r.set(in, in, w - in, h - in);
            c.save();
            clip.reset();
            clip.addRoundRect(r, rad, rad, Path.Direction.CW);
            c.clipPath(clip);
            if (art != null) {
                bgP.setShader(null);
                bgP.setColor(0xFF000000);
                c.drawRect(r, bgP);
                float s = Math.max(r.width() / art.getWidth(), r.height() / art.getHeight());
                float dw = art.getWidth() * s, dh = art.getHeight() * s;
                RectF dst = new RectF(r.centerX() - dw / 2, r.centerY() - dh / 2, r.centerX() + dw / 2,
                                      r.centerY() + dh / 2);
                c.drawBitmap(art, null, dst, bmpP);
            } else if (!empty) {
                float[] hsv = new float[3];
                android.graphics.Color.colorToHSV(color, hsv);
                int c1 = Theme.hsv(hsv[0], 0.62f, 0.50f), c2 = Theme.hsv(hsv[0] + 34, 0.70f, 0.16f);
                bgP.setShader(new LinearGradient(r.left, r.top, r.right, r.bottom, c1, c2, Shader.TileMode.CLAMP));
                c.drawRect(r, bgP);
                bgP.setShader(null);
                for (int i = 1; i <= 5; i++)
                    c.drawCircle(r.right - r.width() * 0.12f, r.bottom - r.height() * 0.1f, r.width() * 0.17f * i,
                                 ringP);
                String s = initials.isEmpty() ? "?" : initials;
                float tw = monoP.measureText(s);
                if (tw > r.width() * 0.8f)
                    monoP.setTextSize(150 * r.width() * 0.8f / tw);
                Paint.FontMetrics fm = monoP.getFontMetrics();
                c.drawText(s, r.centerX() - monoP.measureText(s) / 2, r.centerY() - (fm.ascent + fm.descent) / 2,
                           monoP);
                monoP.setTextSize(150);
            } else {
                bgP.setShader(new LinearGradient(0, r.top, 0, r.bottom, Theme.PANEL_HI, Theme.BG_BOTTOM,
                                                 Shader.TileMode.CLAMP));
                c.drawRect(r, bgP);
                bgP.setShader(null);
                drawIcon(c, Icon.DISC, r.centerX(), r.centerY(), r.width() * 0.36f, iconP);
            }
            c.restore();
            c.drawRoundRect(r, rad, rad, lineP);
        }
    }

    // ---- PadDiagram: the device's buttons and what the game gets ----------------------------------------------------

    /**
     * A generic handheld (screen in the middle, sticks, D-pad, face buttons, shoulders, Start / Select) with, next to
     * each control, the Xbox controller button the game receives (src/input/xinput_device.c). Labels come from the
     * launcher (translated); drawn in code, no maker's art.
     */
    static final class PadDiagram extends View {
        /** Labels, in order: L1, L2, R1, R2, Select, Start, D-pad, left stick, right stick, face buttons, screen. */
        String[] labels = new String[11];
        /** The body's width, a fraction of the view's (narrower in the Controls screen's description column). */
        float bodyFrac = 0.52f;
        private final Paint bodyP = fill(Theme.opaque(Theme.PANEL)), lineP = stroke(Theme.alpha(Theme.TEXT, 0.22f), 2.5f);
        private final Paint ctlP = fill(Theme.blend(Theme.PANEL_HI, Theme.LINE, 0.65f)), ctlLine = stroke(Theme.alpha(Theme.TEXT, 0.35f), 2);
        private final Paint screenP = fill(Theme.BG_BOTTOM), accentP = stroke(Theme.alpha(Theme.ACCENT, 0.75f), 2);
        private final Paint dotP = fill(Theme.ACCENT);
        private final TextPaint keyP = textPaint(24, Theme.MEDIUM, Theme.TEXT);
        private final TextPaint labP = textPaint(Theme.T_SMALL, Theme.MEDIUM, Theme.ACCENT);
        private final TextPaint scrP = textPaint(Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_FAINT);
        private final RectF r = new RectF();

        PadDiagram(Context c) {
            super(c);
        }

        private String lab(int i) {
            return labels[i] == null ? "" : labels[i];
        }

        /** A label at (x, y), anchored left (align 0), centred (1) or right (2), with a dot and a leader line to (tx, ty). */
        private void callout(Canvas c, String s, float x, float y, int align, float tx, float ty) {
            c.drawLine(tx, ty, x, y, accentP);
            c.drawCircle(tx, ty, 5, dotP);
            float w = labP.measureText(s);
            float lx = align == 0 ? x + 12 : align == 1 ? x - w / 2 : x - w - 12;
            float ly = align == 1 ? y - 14 : y + 9;
            c.drawText(s, lx, ly, labP);
        }

        private void key(Canvas c, float cx, float cy, float rad, String s) {
            c.drawCircle(cx, cy, rad, ctlP);
            c.drawCircle(cx, cy, rad, ctlLine);
            Paint.FontMetrics fm = keyP.getFontMetrics();
            c.drawText(s, cx - keyP.measureText(s) / 2, cy - (fm.ascent + fm.descent) / 2, keyP);
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            float bw = Math.min(w * bodyFrac, 640), bh = bw * 0.48f;
            float bx = (w - bw) / 2, by = h * 0.30f;
            // shoulders
            float sw = bw * 0.2f;
            r.set(bx + bw * 0.04f, by - 34, bx + bw * 0.04f + sw, by + 20);
            c.drawRoundRect(r, 22, 22, ctlP);
            c.drawRoundRect(r, 22, 22, ctlLine);
            r.set(bx + bw * 0.96f - sw, by - 34, bx + bw * 0.96f, by + 20);
            c.drawRoundRect(r, 22, 22, ctlP);
            c.drawRoundRect(r, 22, 22, ctlLine);
            // body and screen
            r.set(bx, by, bx + bw, by + bh);
            c.drawRoundRect(r, bh * 0.32f, bh * 0.32f, bodyP);
            c.drawRoundRect(r, bh * 0.32f, bh * 0.32f, lineP);
            float scw = bw * 0.48f, sch = bh * 0.74f;
            r.set(bx + (bw - scw) / 2, by + (bh - sch) / 2, bx + (bw + scw) / 2, by + (bh + sch) / 2);
            c.drawRoundRect(r, 14, 14, screenP);
            c.drawRoundRect(r, 14, 14, ctlLine);
            String scr = fit(lab(10), scrP, r.width() - 24);
            c.drawText(scr, r.centerX() - scrP.measureText(scr) / 2, r.centerY() + 8, scrP);
            float side = (bw - scw) / 2, lcx = bx + side / 2, rcx = bx + bw - side / 2;
            // left: stick above, D-pad below; right: face buttons above, stick below
            float stickR = side * 0.2f, lsy = by + bh * 0.30f, dpy = by + bh * 0.68f;
            c.drawCircle(lcx, lsy, stickR, ctlP);
            c.drawCircle(lcx, lsy, stickR, ctlLine);
            c.drawCircle(lcx, lsy, stickR * 0.55f, ctlLine);
            float d = side * 0.13f, arm = d * 2.6f;
            r.set(lcx - d, dpy - arm, lcx + d, dpy + arm);
            c.drawRoundRect(r, 6, 6, ctlP);
            r.set(lcx - arm, dpy - d, lcx + arm, dpy + d);
            c.drawRoundRect(r, 6, 6, ctlP);
            float fy = by + bh * 0.30f, fo = side * 0.17f, fr = side * 0.085f;
            key(c, rcx, fy - fo, fr, "Y");
            key(c, rcx - fo, fy, fr, "X");
            key(c, rcx + fo, fy, fr, "B");
            key(c, rcx, fy + fo, fr, "A");
            float rsy = by + bh * 0.68f;
            c.drawCircle(rcx, rsy, stickR, ctlP);
            c.drawCircle(rcx, rsy, stickR, ctlLine);
            c.drawCircle(rcx, rsy, stickR * 0.55f, ctlLine);
            // Select / Start below the screen corners
            float ssy = by + bh * 0.93f;
            r.set(bx + side - 70, ssy - 12, bx + side - 20, ssy + 12);
            c.drawRoundRect(r, 12, 12, ctlP);
            c.drawRoundRect(r, 12, 12, ctlLine);
            float selX = r.centerX();
            r.set(bx + bw - side + 20, ssy - 12, bx + bw - side + 70, ssy + 12);
            c.drawRoundRect(r, 12, 12, ctlP);
            c.drawRoundRect(r, 12, 12, ctlLine);
            float staX = r.centerX();

            // callouts: shoulders above, the rest on the sides and below
            float top = by - 34;
            callout(c, lab(0), bx + bw * 0.04f + sw * 0.5f, top - 70, 1, bx + bw * 0.04f + sw * 0.5f, top + 10);
            callout(c, lab(1), bx - 30, top - 18, 2, bx + bw * 0.04f + 10, top + 6);
            callout(c, lab(2), bx + bw * 0.96f - sw * 0.5f, top - 70, 1, bx + bw * 0.96f - sw * 0.5f, top + 10);
            callout(c, lab(3), bx + bw + 30, top - 18, 0, bx + bw * 0.96f - 10, top + 6);
            callout(c, lab(4), selX - 40, by + bh + 60, 2, selX, ssy);
            callout(c, lab(5), staX + 40, by + bh + 60, 0, staX, ssy);
            callout(c, lab(6), bx - 30, dpy, 2, lcx - arm, dpy);
            callout(c, lab(7), bx - 30, lsy, 2, lcx - stickR, lsy);
            callout(c, lab(8), bx + bw + 30, rsy, 0, rcx + stickR, rsy);
            callout(c, lab(9), bx + bw + 30, fy, 0, rcx + fo + fr, fy);
        }
    }
    // ---- Chip: a status (a dot and its label) ------------------------------------------------------------------------

    /** A status worth saying (not « Ready »): a dot in its colour and its label in TEXT_DIM. No pill. */
    static final class Chip extends View {
        private String label = "";
        private int color = Theme.ACCENT;
        private final Paint dotP = fill(Theme.ACCENT);
        private final TextPaint tp = textPaint(Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM);

        String label() {
            return label;
        }

        int color() {
            return color;
        }

        Chip(Context c) {
            super(c);
        }

        void set(String label, int color) {
            this.label = label == null ? "" : label;
            this.color = color;
            requestLayout();
            invalidate();
        }

        @Override
        protected void onMeasure(int w, int h) {
            Paint.FontMetrics fm = tp.getFontMetrics();
            setMeasuredDimension(label.isEmpty() ? 0 : Math.round(tp.measureText(label) + 12 + Theme.S2),
                                 Math.round(fm.descent - fm.ascent) + 4);
        }

        @Override
        protected void onDraw(Canvas c) {
            if (label.isEmpty())
                return;
            float h = getHeight();
            dotP.setColor(color);
            c.drawCircle(6, h / 2, 6, dotP);
            Paint.FontMetrics fm = tp.getFontMetrics();
            c.drawText(label, 12 + Theme.S2, h / 2 - (fm.ascent + fm.descent) / 2, tp);
        }
    }

    // ---- Hint: a controller button and what it does (also a touch target) ----------------------------------------

    /** One discreet hint: the button's glyph in a 2 px TEXT_DIM outline, and its label. */
    static final class Hint extends View {
        private final String glyph, label;
        private final Paint ring = stroke(Theme.TEXT_DIM, 2);
        private final TextPaint ink = textPaint(Theme.T_SMALL - 2, Theme.MEDIUM, Theme.TEXT_DIM);
        private final TextPaint tp = textPaint(Theme.T_HINT, Theme.REGULAR, Theme.TEXT_DIM);
        private final RectF r = new RectF();
        private static final float D = 40;

        Hint(Context c, String glyph, String label, View.OnClickListener click) {
            super(c);
            this.glyph = glyph;
            this.label = label;
            setOnClickListener(click);
            setFocusable(false);
        }

        String labelForTest() {
            return label;
        }

        String glyphForTest() {
            return glyph;
        }

        private float glyphW() {
            return glyph.length() <= 1 ? D : Math.max(D, ink.measureText(glyph) + 26);
        }

        @Override
        protected void onMeasure(int w, int h) {
            setMeasuredDimension(Math.round(glyphW() + 12 + tp.measureText(label) + 4), Theme.FOOTER_H);
        }

        @Override
        protected void onDraw(Canvas c) {
            float h = getHeight(), gw = glyphW();
            int col = isPressed() ? Theme.ACCENT : Theme.TEXT_DIM;
            ring.setColor(col);
            ink.setColor(col);
            r.set(1, h / 2 - D / 2 + 1, gw - 1, h / 2 + D / 2 - 1);
            c.drawRoundRect(r, D / 2, D / 2, ring);
            Paint.FontMetrics fm = ink.getFontMetrics();
            c.drawText(glyph, gw / 2 - ink.measureText(glyph) / 2, h / 2 - (fm.ascent + fm.descent) / 2, ink);
            Paint.FontMetrics lm = tp.getFontMetrics();
            c.drawText(label, gw + 12, h / 2 - (lm.ascent + lm.descent) / 2, tp);
        }

        @Override
        protected void drawableStateChanged() {
            super.drawableStateChanged();
            invalidate();
        }
    }

    // ---- Ring: the import progress -----------------------------------------------------------------------------------

    /** A plain progress ring (the import): a LINE track, an arc in its colour, the percentage and two lines. */
    static final class Ring extends View {
        private float frac = -1;
        private String big = "", label = "", sub = "";
        private final Paint trackP = stroke(Theme.LINE, 12), arcP = stroke(Theme.ACCENT, 12);
        private final TextPaint bigP = textPaint(Theme.T_DISPLAY * 1.5f, Theme.MEDIUM, Theme.TEXT);
        private final TextPaint labelP = textPaint(Theme.T_BODY, Theme.REGULAR, Theme.TEXT_DIM);
        private final TextPaint subP = textPaint(Theme.T_SMALL, Theme.REGULAR, Theme.TEXT_DIM);
        private final RectF r = new RectF();

        Ring(Context c) {
            super(c);
        }

        /** The arc's colour. */
        void setColor(int color) {
            arcP.setColor(color);
            invalidate();
        }

        /** frac below 0: indeterminate (a turning arc). */
        void set(float frac, String big, String label, String sub) {
            this.frac = frac;
            this.big = big == null ? "" : big;
            this.label = label == null ? "" : label;
            this.sub = sub == null ? "" : sub;
            invalidate();
        }

        @Override
        protected void onDraw(Canvas c) {
            float cx = getWidth() / 2f, cy = getHeight() / 2f;
            float R = Math.min(getWidth(), getHeight()) / 2f - 24;
            r.set(cx - R, cy - R, cx + R, cy + R);
            c.drawArc(r, 0, 360, false, trackP);
            if (frac >= 0) {
                if (frac > 0.002f)
                    c.drawArc(r, -90, 360 * Math.min(1, frac), false, arcP);
            } else {
                float start = (SystemClock.uptimeMillis() % 1400) * 360f / 1400f - 90;
                c.drawArc(r, start, 80, false, arcP);
                postInvalidateOnAnimation();
            }
            Paint.FontMetrics bf = bigP.getFontMetrics();
            float by = cy - 18;
            c.drawText(big, cx - bigP.measureText(big) / 2, by, bigP);
            float ly = by - bf.descent + 58;
            String l = fit(label, labelP, R * 1.5f);
            c.drawText(l, cx - labelP.measureText(l) / 2, ly, labelP);
            String s = fit(sub, subP, R * 1.4f);
            c.drawText(s, cx - subP.measureText(s) / 2, ly + 42, subP);
        }
    }

    // ---- Meter: a thin bar (storage) ---------------------------------------------------------------------------------

    static final class Meter extends View {
        private float frac;
        private int color = Theme.ACCENT;
        private final Paint track = fill(Theme.LINE), bar = fill(Theme.ACCENT);

        Meter(Context c) {
            super(c);
        }

        void set(float frac, int color) {
            this.frac = Math.max(0, Math.min(1, frac));
            this.color = color;
            invalidate();
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            c.drawRoundRect(0, 0, w, h, h / 2, h / 2, track);
            bar.setColor(color);
            if (frac > 0)
                c.drawRoundRect(0, 0, Math.max(h, w * frac), h, h / 2, h / 2, bar);
        }
    }
}
