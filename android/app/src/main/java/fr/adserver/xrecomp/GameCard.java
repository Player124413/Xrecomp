package fr.adserver.xrecomp;

import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.text.TextPaint;
import android.view.View;

/**
 * One game of the library grid (docs/android/app-shell.md « Game library »): its title image (TitleArt's cached PNG)
 * or a monogram on the title ID's hue, its title on one line, and under it its state (GameState) as a dot and a label
 * — not for a plain « Ready » (a game the app can prepare says Prepared, Prepared · out of date or Not prepared);
 * when the library holds games of several consoles, the console's short name as a badge in the art's top left corner (XBA-178); a favourite's star in its top right one (XBA-206). Focusable: it grows a little when focused, and the launcher's Glide draws a ring
 * around its art. Colours are read from Theme when drawn (the library is always in the stock colours).
 */
final class GameCard extends View {
    /** The art square, in design pixels, and the card's size (the title may use the gap after the art). */
    static final int ART = 288, W = 336, H = 416;

    private Bitmap art;
    private String initials = "";
    private int monoColor = Theme.ACCENT_DEEP;
    private String title = "", chip = "", badge = "";
    private int chipColor = Theme.TEXT_DIM;
    private boolean chipShown = true, dim, favourite;
    private float focusT;
    private ValueAnimator anim;

    private final Paint fillP = new Paint(Paint.ANTI_ALIAS_FLAG), lineP = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bmpP = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final TextPaint titleP = new TextPaint(Paint.ANTI_ALIAS_FLAG), chipP = new TextPaint(Paint.ANTI_ALIAS_FLAG),
            monoP = new TextPaint(Paint.ANTI_ALIAS_FLAG), badgeP = new TextPaint(Paint.ANTI_ALIAS_FLAG);
    private final RectF artR = new RectF();
    private final Path clip = new Path();

    GameCard(Context c) {
        super(c);
        setFocusable(true);
        setClickable(true);
        lineP.setStyle(Paint.Style.STROKE);
        lineP.setStrokeWidth(2);
        titleP.setTextSize(Theme.T_BODY);
        titleP.setTypeface(Theme.MEDIUM);
        chipP.setTextSize(Theme.T_SMALL);
        chipP.setTypeface(Theme.REGULAR);
        monoP.setTypeface(Theme.MEDIUM);
        badgeP.setTextSize(Theme.T_SMALL);
        badgeP.setTypeface(Theme.MEDIUM);
    }

    /** The console badge (« GBA »), "" or null for none. */
    void setBadge(String b) {
        badge = b == null ? "" : b;
        invalidate();
    }

    /** XBA-206: a favourite, a star in the art's top right corner. */
    void setFavourite(boolean f) {
        favourite = f;
        invalidate();
    }

    boolean favouriteForTest() {
        return favourite;
    }

    String badgeForTest() {
        return badge;
    }

    /** A game: its art (null: the monogram of title on the colour of key), its title and its state. */
    GameCard setGame(Bitmap art, String key, String title, String chip, int chipColor) {
        this.art = art;
        this.title = title == null ? "" : title;
        this.initials = TitleArt.initials(this.title);
        this.monoColor = TitleArt.monogramColor(key == null ? this.title : key);
        this.chip = chip == null ? "" : chip;
        this.chipColor = chipColor;
        invalidate();
        return this;
    }

    /** Whether the state line shows (not for a plain « Ready »: only a state worth saying is drawn). */
    void setChipShown(boolean shown) {
        chipShown = shown;
        invalidate();
    }

    void setDim(boolean d) {
        dim = d;
        invalidate();
    }

    void setChip(String s, int color) {
        chip = s == null ? "" : s;
        chipColor = color;
        invalidate();
    }

    String titleForTest() {
        return title;
    }

    String chipForTest() {
        return chip;
    }

    boolean chipShownForTest() {
        return chipShown && !chip.isEmpty();
    }

    boolean hasArtForTest() {
        return art != null;
    }

    /** The art square in the card's own coordinates (the Glide's ring goes around it). */
    void artRect(RectF out) {
        float side = Math.min(ART, getWidth());
        out.set(0, 8, side, 8 + side);
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

    @Override
    protected void onDraw(Canvas c) {
        float w = getWidth(), t = focusT;
        artRect(artR);
        float side = artR.width(), rad = Theme.ART_RADIUS;
        clip.reset();
        clip.addRoundRect(artR, rad, rad, Path.Direction.CW);
        c.save();
        c.clipPath(clip);
        if (art != null) {
            fillP.setShader(null);
            fillP.setColor(0xFF000000);
            c.drawRect(artR, fillP);
            float s = Math.max(artR.width() / art.getWidth(), artR.height() / art.getHeight());
            float dw = art.getWidth() * s, dh = art.getHeight() * s;
            c.drawBitmap(art, null, new RectF(artR.centerX() - dw / 2, artR.centerY() - dh / 2,
                                              artR.centerX() + dw / 2, artR.centerY() + dh / 2), bmpP);
        } else {
            float[] hsv = new float[3];
            android.graphics.Color.colorToHSV(monoColor, hsv);
            int c1 = Theme.hsv(hsv[0], 0.62f, 0.50f), c2 = Theme.hsv(hsv[0] + 34, 0.70f, 0.16f);
            fillP.setShader(new LinearGradient(artR.left, artR.top, artR.right, artR.bottom, c1, c2,
                                               Shader.TileMode.CLAMP));
            c.drawRect(artR, fillP);
            fillP.setShader(null);
            String s = initials.isEmpty() ? "?" : initials;
            monoP.setColor(Theme.alpha(Theme.TEXT_BRIGHT, 0.94f));
            monoP.setTextSize(side * 0.38f);
            float tw = monoP.measureText(s);
            if (tw > side * 0.8f)
                monoP.setTextSize(side * 0.38f * side * 0.8f / tw);
            Paint.FontMetrics fm = monoP.getFontMetrics();
            c.drawText(s, artR.centerX() - monoP.measureText(s) / 2, artR.centerY() - (fm.ascent + fm.descent) / 2,
                       monoP);
        }
        if (dim || isPressed()) {
            fillP.setShader(null);
            fillP.setColor(dim ? 0x88000000 : Theme.alpha(Theme.ACCENT, 0.12f));
            c.drawRect(artR, fillP);
        }
        if (!badge.isEmpty()) {
            // A dark pill with the console's name, in the art's top left corner (readable over any art).
            Paint.FontMetrics bm = badgeP.getFontMetrics();
            float bw = badgeP.measureText(badge) + 2 * Theme.S2, bh = bm.descent - bm.ascent + Theme.S1;
            RectF b = new RectF(artR.left + Theme.S2, artR.top + Theme.S2, artR.left + Theme.S2 + bw,
                                artR.top + Theme.S2 + bh);
            fillP.setShader(null);
            fillP.setColor(0xB0000000);
            c.drawRoundRect(b, bh / 2, bh / 2, fillP);
            badgeP.setColor(Theme.alpha(Theme.TEXT_BRIGHT, dim ? 0.6f : 0.94f));
            c.drawText(badge, b.left + Theme.S2, b.centerY() - (bm.ascent + bm.descent) / 2, badgeP);
        }
        if (favourite) {
            // A star on a dark disc, in the art's top right corner (XBA-206).
            float r = 22, cx = artR.right - Theme.S2 - r, cy = artR.top + Theme.S2 + r;
            fillP.setShader(null);
            fillP.setColor(0xB0000000);
            c.drawCircle(cx, cy, r, fillP);
            fillP.setColor(Theme.alpha(Theme.ACCENT, dim ? 0.6f : 1f));
            c.drawPath(Widgets.star(cx, cy + 1, r * 0.68f), fillP);
        }
        c.restore();
        lineP.setColor(Theme.alpha(Theme.TEXT, 0.10f));
        c.drawRoundRect(artR, rad, rad, lineP);

        // Title: one line; the state under it, when there is one worth saying.
        float tw = w - 8;
        Paint.FontMetrics tf = titleP.getFontMetrics();
        float ty = artR.bottom + Theme.S3 - tf.ascent;
        titleP.setColor(dim ? Theme.TEXT_FAINT : Theme.blend(Theme.TEXT, Theme.TEXT_BRIGHT, t));
        c.drawText(Widgets.fit(title, titleP, tw), 0, ty, titleP);
        if (chipShown && !chip.isEmpty()) {
            Paint.FontMetrics fm = chipP.getFontMetrics();
            float cy = ty + tf.descent + Theme.S2 + (fm.descent - fm.ascent) / 2;
            fillP.setShader(null);
            fillP.setColor(chipColor);
            c.drawCircle(7, cy, 7, fillP);
            chipP.setColor(dim ? Theme.TEXT_FAINT : Theme.TEXT_DIM);
            c.drawText(Widgets.fit(chip, chipP, tw - 26), 26, cy - (fm.ascent + fm.descent) / 2, chipP);
        }
    }
}
