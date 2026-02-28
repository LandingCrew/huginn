/**
 * Intuition Widget - Huginn contextual quick-access HUD
 * ActionScript 2.0 for Skyrim Scaleform
 *
 * Public API (called from C++ via GFxValue::Invoke):
 *   setSlot(index, name, type, confidence)  - Update a slot
 *   clearSlot(index)                        - Hide a slot
 *   setSlotCount(count)                     - Set visible slot count + resize
 *   setPage(current, total, name)           - Update page indicator
 *   setUrgent(index, active)                - Enable/disable pulse on slot
 *   setWidgetAlpha(alpha)                   - Overall widget opacity (0-100)
 *   setChildAlpha(alpha)                    - Set secondary element opacity (0-100)
 *   setRefreshEffect(mode)                  - Refresh effect: 0=none, 1=pulse, 2=tint
 *   setRefreshStrength(pct)                 - Refresh effect strength (0-100%)
 *   setSlotEffect(mode)                     - Slot change anim: 0=slide, 1=fade, 2=instant
 *
 * Animations:
 *   - Slide reveal: when slot content changes, old text slides up + fades out,
 *     new text rises in from below with ease-out deceleration.
 *   - Refresh effect: when ANY slot changes, idle slots signal a collective
 *     refresh. Mode: tint (color shift to black), pulse (alpha dip), or none.
 *   - Urgent pulse: override slots do a slow sine pulse.
 */
class Intuition extends MovieClip
{
    // ── Layout constants ──────────────────────────────────────
    private static var MAX_SLOTS:Number    = 10;
    private static var SLOT_HEIGHT:Number  = 20;
    private static var SLOT_WIDTH:Number   = 220;
    private static var PADDING:Number      = 10;
    private static var KEY_LABEL_W:Number  = 20;
    private static var NAME_X:Number       = 28;    // After key label + gap
    private static var DOT_RADIUS:Number   = 3;
    private static var DOT_GAP:Number      = 4;    // Consistent gap between all pip elements
    private static var MAX_DOTS:Number     = 10;   // Matches SlotSettings::MAX_PAGES

    // ── Visual State enum (must match C++ SlotVisualState) ──
    private static var STATE_NORMAL:Number      = 0;
    private static var STATE_CONFIRMED:Number   = 1;
    private static var STATE_EXPIRING:Number    = 2;
    private static var STATE_OVERRIDE:Number    = 3;
    private static var STATE_WILDCARD:Number    = 4;

    // ── Animation constants ──────────────────────────────────
    private static var ANIM_OUT_SEC:Number     = 0.30;  // Slide-out duration
    private static var ANIM_IN_SEC:Number      = 0.35;  // Slide-in duration
    private static var ANIM_SLIDE_PX:Number    = 14;    // Vertical slide distance
    private static var FLASH_SEC:Number        = 0.50;  // Refresh flash duration

    // ── Confirm flash timing ──
    private static var CONFIRM_FLASH_SEC:Number = 0.5;   // Single dip duration
    private static var EXPIRING_CYCLE_SEC:Number = 2.5;  // Slow pulse period

    // ── Colors ────────────────────────────────────────────────
    private static var COLOR_SPELL_TOP:Number   = 0xFFD4A0;  // Warm white (slot 0)
    private static var COLOR_SPELL:Number       = 0xFFFFFF;  // Pure white
    private static var COLOR_WILDCARD:Number    = 0x7EB8FF;  // Soft blue
    private static var COLOR_HEALTH_POT:Number  = 0xFF6666;  // Soft red
    private static var COLOR_MAGICKA_POT:Number = 0x6699FF;  // Soft blue
    private static var COLOR_STAMINA_POT:Number = 0x66FF66;  // Soft green
    private static var COLOR_WEAPON:Number      = 0xE6B84D;  // Warm gold
    private static var COLOR_EMPTY:Number       = 0x808080;  // Gray
    private static var COLOR_KEY:Number         = 0x999999;  // Dim

    // ── Type enum (must match C++ IntuitionSlotType) ─────────
    private static var TYPE_EMPTY:Number          = 0;
    private static var TYPE_NOMATCH:Number        = 1;
    private static var TYPE_SPELL:Number          = 2;
    private static var TYPE_WILDCARD:Number       = 3;
    private static var TYPE_HEALTH_POTION:Number  = 4;
    private static var TYPE_MAGICKA_POTION:Number = 5;
    private static var TYPE_STAMINA_POTION:Number = 6;
    private static var TYPE_MELEE_WEAPON:Number   = 7;
    private static var TYPE_RANGED_WEAPON:Number  = 8;

    // ── Instance references ───────────────────────────────────
    private var _background:MovieClip;
    private var _slotContainer:MovieClip;
    private var _pageContainer:MovieClip;
    private var _slotClips:Array;
    private var _activeSlotCount:Number;

    // ── Pulse state ───────────────────────────────────────────
    private var _urgentSlots:Array;     // Boolean per slot (deprecated - replaced by visual states)
    private var _pulseTime:Number;

    // ── Visual state tracking ────────────────────────────────
    private var _visualState:Array;     // SlotVisualState per slot
    private var _confirmTimer:Array;    // Confirm flash countdown per slot

    // ── Animation state ──────────────────────────────────────
    private var _animPhase:Array;       // 0=idle, 1=slideOut, 2=slideIn
    private var _animTimer:Array;       // Seconds elapsed in current phase
    private var _pendingName:Array;     // Queued name for after slideOut
    private var _pendingType:Array;     // Queued type
    private var _pendingConf:Array;     // Queued confidence
    private var _currentName:Array;     // Last displayed name per slot
    private var _currentDetail:Array;   // Last displayed detail per slot
    private var _pendingDetail:Array;   // Queued detail for after slideOut
    private var _baseAlpha:Array;       // Base alpha per slot (for proportional flash)
    private var _slotReady:Array;       // true after first setSlot (suppresses initial anim)
    private var _flashTimer:Number;     // Global refresh flash countdown

    private var _childAlpha:Number;      // INI: secondary element opacity (0-100)

    // ── Slot effect state ──────────────────────────────────────────
    private var _slotMode:Number;        // 0=slide, 1=fade, 2=instant

    // ── Refresh effect state ────────────────────────────────────
    private var _refreshMode:Number;     // 0=none, 1=pulse, 2=tint
    private var _baseColor:Array;        // Base text color per slot (for tint restore)
    private var _tintDirty:Boolean;      // True while tint is active, cleared after restore

    // ── Effect strength (from INI, 0-100 → 0.0-1.0) ─────────
    private var _refreshStrength:Number; // Max blend/dip for refresh effect

    // ── Tint target ──────────────────────────────────────────────
    private static var TINT_COLOR:Number = 0x000000;  // Black (darkens each slot's own color)

    // ── Dynamic width tracking ─────────────────────────────────
    private var _bgWidth:Number;         // Current background width (auto-sized to content)
    private var _pipStripWidth:Number;   // Total width of page indicator strip (for centering)

    // ── Font format ───────────────────────────────────────────
    private var _nameFormat:TextFormat;
    private var _keyFormat:TextFormat;

    // ===========================================================
    //  Entry point — called by mtasc -main on frame 1
    // ===========================================================

    static function main():Void
    {
        Object.registerClass("IntuitionWidget", Intuition);
        _root.attachMovie("IntuitionWidget", "widget", 1);
    }

    // ===========================================================
    //  Constructor (runs when attachMovie creates the instance)
    // ===========================================================

    function Intuition()
    {
        _slotClips = [];
        _urgentSlots = [];
        _activeSlotCount = 0;
        _pulseTime = 0;

        // Animation arrays
        _animPhase = [];
        _animTimer = [];
        _pendingName = [];
        _pendingType = [];
        _pendingConf = [];
        _currentName = [];
        _currentDetail = [];
        _pendingDetail = [];
        _baseAlpha = [];
        _slotReady = [];
        _flashTimer = 0;

        // Visual state tracking
        _visualState = [];       // Visual state per slot
        _confirmTimer = [];      // Confirm flash countdown per slot

        _childAlpha = 70;        // Default; C++ can override via setChildAlpha()

        // Slot effect state
        _slotMode = 0;           // Default slide; C++ can override via setSlotEffect()

        // Refresh effect state
        _refreshMode = 2;        // Default tint; C++ can override via setRefreshEffect()
        _baseColor = [];
        _tintDirty = false;

        // Effect strength (INI percentage → 0.0-1.0 fraction)
        _refreshStrength = 0.15; // Default 15%; C++ can override via setRefreshStrength()
        _bgWidth = SLOT_WIDTH + PADDING * 2;  // Initial width; auto-resizes on content
        _pipStripWidth = 0;

        // Skyrim's standard UI font
        _nameFormat = new TextFormat();
        _nameFormat.font = "$EverywhereMediumFont";
        _nameFormat.size = 12;

        _keyFormat = new TextFormat();
        _keyFormat.font = "$EverywhereMediumFont";
        _keyFormat.size = 11;

        buildBackground();
        buildSlots();
        buildPageIndicator();

        // Start with nothing visible
        _background._visible = false;

        // Animation tick is driven from C++ via AdvanceMovie() calling
        // tick(dt) directly — Scaleform GFx in Skyrim does not reliably
        // fire AS2 onEnterFrame events.
    }

    // ===========================================================
    //  Public API - called from C++ via GFxValue::Invoke
    // ===========================================================

    /**
     * Update a slot with new content.
     * Detects name changes and triggers slide reveal animation.
     * Visual state controls per-slot animation effects.
     */
    public function setSlot(index:Number, name:String,
                            type:Number, confidence:Number,
                            detail:String, visualState:Number):Void
    {
        if (index < 0 || index >= MAX_SLOTS) return;
        if (detail == undefined) detail = "";
        if (visualState == undefined) visualState = STATE_NORMAL;

        var slot:MovieClip = _slotClips[index];
        slot._visible = true;

        // Key label (static — always set immediately, never animates)
        var kf:TextField = slot.keyLabel;
        kf.text = String(index + 1);
        kf.textColor = COLOR_KEY;
        _keyFormat.color = COLOR_KEY;
        kf.setTextFormat(_keyFormat);

        // First population: apply directly, no animation
        if (!_slotReady[index]) {
            _slotReady[index] = true;
            _currentName[index] = name;
            _currentDetail[index] = detail;
            _visualState[index] = visualState;
            applyItemContent(index, name, type, confidence, detail);
            _background._visible = true;
            return;
        }

        // Detect visual state transitions
        var prevState:Number = _visualState[index];
        _visualState[index] = visualState;

        // CONFIRMED: Trigger single flash on state entry
        if (visualState == STATE_CONFIRMED && prevState != STATE_CONFIRMED) {
            _confirmTimer[index] = CONFIRM_FLASH_SEC;
        }

        // Same name: update detail only
        if (name == _currentName[index]) {
            if (detail != _currentDetail[index]) {
                _currentDetail[index] = detail;
                updateDisplayText(index, name, type, detail);
            }

            // Only reset alpha when no active effects
            var hasEffect:Boolean = (_animPhase[index] != 0) ||
                                    (_flashTimer > 0) ||
                                    (_confirmTimer[index] > 0) ||
                                    (_visualState[index] == STATE_OVERRIDE) ||
                                    (_visualState[index] == STATE_WILDCARD) ||
                                    (_visualState[index] == STATE_EXPIRING);
            if (!hasEffect) {
                _slotClips[index].itemName._alpha = getAlphaForType(type);
                _slotClips[index].itemName._y = 2;
                _baseAlpha[index] = getAlphaForType(type);
            }
            _background._visible = true;
            return;
        }

        // Content changed — trigger slide animation
        // (refresh flash removed — now handled by per-slot states)

        if (_slotMode == 2) {
            // Instant mode: swap content immediately, no animation
            _currentName[index] = name;
            _currentDetail[index] = detail;
            applyItemContent(index, name, type, confidence, detail);
        } else {
            // Slide or Fade: queue animation
            _pendingName[index] = name;
            _pendingType[index] = type;
            _pendingConf[index] = confidence;
            _pendingDetail[index] = detail;

            if (_animPhase[index] == 0) {
                _animPhase[index] = 1;
                _animTimer[index] = 0;
            }
            // If already animating, pending data is updated and will be
            // applied when current animation reaches the swap point.
        }

        _background._visible = true;
    }

    /** Hide a single slot and reset its animation state. */
    public function clearSlot(index:Number):Void
    {
        if (index < 0 || index >= MAX_SLOTS) return;
        _slotClips[index]._visible = false;
        _animPhase[index] = 0;
        _animTimer[index] = 0;
        _slotReady[index] = false;
        _currentName[index] = "";
        _currentDetail[index] = "";
    }

    /** Set how many slots are active, resize background to fit. */
    public function setSlotCount(count:Number):Void
    {
        _activeSlotCount = Math.min(count, MAX_SLOTS);

        for (var i:Number = 0; i < MAX_SLOTS; i++) {
            _slotClips[i]._visible = (i < _activeSlotCount);
            if (i >= _activeSlotCount) {
                _animPhase[i] = 0;
                _animTimer[i] = 0;
            }
        }

        resizeBackground();
        _background._visible = (_activeSlotCount > 0);
    }

    /**
     * Update page indicator dots with inline page name label.
     * Layout: ○ ○ ●[PageName] ○  — label appears after the active dot.
     * The whole strip is re-centered above the background each call.
     */
    public function setPage(current:Number, total:Number, name:String):Void
    {
        var lbl:TextField = _pageContainer.pageLabel;
        var curX:Number = 0;

        lbl._visible = false;

        for (var i:Number = 0; i < MAX_DOTS; i++) {
            var dot:MovieClip = _pageContainer["dot" + i];
            if (i < total) {
                dot._visible = true;
                dot._x = curX;
                dot._alpha = (i == current) ? 100 : _childAlpha;

                // Advance cursor past this dot's right edge
                curX += DOT_RADIUS + DOT_GAP;

                if (i == current && name.length > 0) {
                    // Insert label between this dot and the next
                    lbl.text = name;
                    lbl.textColor = 0xFFFFFF;
                    _keyFormat.color = 0xFFFFFF;
                    lbl.setTextFormat(_keyFormat);
                    lbl._alpha = _childAlpha;
                    lbl._x = curX;
                    lbl._visible = true;
                    // textWidth + 4 accounts for AS2 TextField gutter (2px per side)
                    curX += lbl.textWidth + 4 + DOT_GAP;
                }

                // Add space for the next dot's left edge
                curX += DOT_RADIUS;
            } else {
                dot._visible = false;
            }
        }

        // Left-justify, aligned with slot key labels
        _pipStripWidth = curX;
        _pageContainer._x = PADDING;

        // Only show page indicator when multiple pages exist
        _pageContainer._visible = (total > 1);
    }

    /** Enable/disable urgent pulse on a slot. */
    public function setUrgent(index:Number, active:Boolean):Void
    {
        if (index < 0 || index >= MAX_SLOTS) return;
        _urgentSlots[index] = active;
    }

    /** Set overall widget opacity (0-100). */
    public function setWidgetAlpha(alpha:Number):Void
    {
        this._alpha = alpha;
    }

    /** Set child element opacity (0-100) for secondary elements like page labels. */
    public function setChildAlpha(alpha:Number):Void
    {
        _childAlpha = alpha;
    }

    /** Set refresh effect mode (0=none, 1=flash, 2=tint). */
    public function setRefreshEffect(mode:Number):Void
    {
        _refreshMode = mode;
    }

    /** Set slot content change effect (0=slide, 1=fade, 2=instant). */
    public function setSlotEffect(mode:Number):Void
    {
        _slotMode = mode;
    }

    /** Set refresh effect strength (0-100%). Controls max tint blend or alpha dip. */
    public function setRefreshStrength(pct:Number):Void
    {
        _refreshStrength = pct / 100;
    }

    // ===========================================================
    //  Construction helpers
    // ===========================================================

    private function buildBackground():Void
    {
        _background = this.createEmptyMovieClip("background", 0);
        drawBackground(3);
    }

    private function drawBackground(slotCount:Number):Void
    {
        var h:Number = (slotCount * SLOT_HEIGHT) + PADDING * 2;

        _background.clear();
        // Semi-transparent black panel
        _background.beginFill(0x000000, 40);
        _background.moveTo(0, 0);
        _background.lineTo(_bgWidth, 0);
        _background.lineTo(_bgWidth, h);
        _background.lineTo(0, h);
        _background.lineTo(0, 0);
        _background.endFill();

        // Subtle border
        _background.lineStyle(1, 0xFFFFFF, 15);
        _background.moveTo(0, 0);
        _background.lineTo(_bgWidth, 0);
        _background.lineTo(_bgWidth, h);
        _background.lineTo(0, h);
        _background.lineTo(0, 0);
    }

    private function buildSlots():Void
    {
        _slotContainer = this.createEmptyMovieClip("slotContainer", 1);
        _slotContainer._x = PADDING;
        _slotContainer._y = PADDING;

        for (var i:Number = 0; i < MAX_SLOTS; i++) {
            var depth:Number = i;
            var slot:MovieClip = _slotContainer.createEmptyMovieClip("slot" + i, depth);
            slot._y = i * SLOT_HEIGHT;
            slot._visible = false;

            // Key label: "1", "2", etc.
            slot.createTextField("keyLabel", 0, 0, 2, KEY_LABEL_W, SLOT_HEIGHT);
            var kf:TextField = slot.keyLabel;
            kf.selectable = false;
            kf.textColor = COLOR_KEY;
            kf.embedFonts = false;
            kf.setNewTextFormat(_keyFormat);

            // Item name (autoSize so text is never clipped)
            slot.createTextField("itemName", 1, NAME_X, 2, 400, SLOT_HEIGHT);
            var nf:TextField = slot.itemName;
            nf.selectable = false;
            nf.textColor = COLOR_SPELL;
            nf.embedFonts = false;
            nf.autoSize = "left";
            nf.setNewTextFormat(_nameFormat);

            _slotClips.push(slot);
            _urgentSlots.push(false);

            // Animation state
            _animPhase.push(0);
            _animTimer.push(0);
            _pendingName.push("");
            _pendingType.push(0);
            _pendingConf.push(0);
            _currentName.push("");
            _currentDetail.push("");
            _pendingDetail.push("");
            _baseAlpha.push(100);
            _slotReady.push(false);

            // Visual state tracking
            _visualState.push(STATE_NORMAL);
            _confirmTimer.push(0);

            // Refresh effect: base color for tint restore
            _baseColor.push(COLOR_SPELL);
        }
    }

    private function buildPageIndicator():Void
    {
        _pageContainer = this.createEmptyMovieClip("pageIndicator", 2);
        _pageContainer._y = -14;
        _pageContainer._visible = false;

        for (var i:Number = 0; i < MAX_DOTS; i++) {
            var dot:MovieClip = _pageContainer.createEmptyMovieClip("dot" + i, i);

            // Draw diamond shape as page dot
            dot.beginFill(0xFFFFFF, 80);
            dot.moveTo(0, -DOT_RADIUS);
            dot.lineTo(DOT_RADIUS, 0);
            dot.lineTo(0, DOT_RADIUS);
            dot.lineTo(-DOT_RADIUS, 0);
            dot.lineTo(0, -DOT_RADIUS);
            dot.endFill();

            dot._visible = false;
        }

        // Page name label — positioned dynamically by setPage()
        _pageContainer.createTextField("pageLabel", 10, 0, -7, 120, 16);
        var lbl:TextField = _pageContainer.pageLabel;
        lbl.selectable = false;
        lbl.textColor = 0xFFFFFF;
        lbl.embedFonts = false;
        lbl.autoSize = "left";
        lbl.setNewTextFormat(_keyFormat);
        lbl._visible = false;
    }

    // ===========================================================
    //  Animation tick
    // ===========================================================

    /**
     * Animation tick — called from C++ AdvanceMovie() every render frame.
     * @param dtArg Delta time in seconds from the game engine
     */
    public function tick(dtArg:Number):Void
    {
        var dt:Number = dtArg;
        if (dt > 0.1) dt = 0.1;
        if (dt <= 0) return;

        _pulseTime += dt;

        for (var i:Number = 0; i < _activeSlotCount; i++) {

            // ── Per-slot slide animation ─────────────────────
            if (_animPhase[i] == 1) {
                // Phase 1: out — old content disappears
                _animTimer[i] += dt;
                var t1:Number = _animTimer[i] / ANIM_OUT_SEC;
                if (t1 > 1) t1 = 1;

                var nf1:TextField = _slotClips[i].itemName;
                if (_slotMode == 0) {
                    // Slide: move up + fade
                    nf1._y = 2 - (t1 * ANIM_SLIDE_PX);
                }
                nf1._alpha = _baseAlpha[i] * (1 - t1);

                if (t1 >= 1) {
                    // Swap to new content at midpoint
                    _currentName[i] = _pendingName[i];
                    _currentDetail[i] = _pendingDetail[i];
                    applyItemContent(i, _pendingName[i],
                                     _pendingType[i], _pendingConf[i],
                                     _pendingDetail[i]);
                    _animPhase[i] = 2;
                    _animTimer[i] = 0;
                    if (_slotMode == 0) {
                        // Slide: position below for rise-in
                        _slotClips[i].itemName._y = 2 + ANIM_SLIDE_PX;
                    }
                    _slotClips[i].itemName._alpha = 0;
                }
            }
            else if (_animPhase[i] == 2) {
                // Phase 2: in — new content appears
                _animTimer[i] += dt;
                var t2:Number = _animTimer[i] / ANIM_IN_SEC;
                if (t2 > 1) t2 = 1;

                // Ease-out: decelerates into final position
                var ease:Number = 1 - (1 - t2) * (1 - t2);

                var nf2:TextField = _slotClips[i].itemName;
                if (_slotMode == 0) {
                    // Slide: rise from below
                    nf2._y = 2 + ANIM_SLIDE_PX * (1 - ease);
                }
                nf2._alpha = _baseAlpha[i] * ease;

                if (t2 >= 1) {
                    _animPhase[i] = 0;
                    nf2._y = 2;
                    nf2._alpha = _baseAlpha[i];
                }
            }
            else {
                // ── Idle: priority order of visual effects ────

                // Priority 1: Override/Wildcard urgent pulse (2s cycle, 60-100% alpha)
                if (_visualState[i] == STATE_OVERRIDE || _visualState[i] == STATE_WILDCARD) {
                    var pulse:Number = 0.6 + 0.4 * Math.sin(_pulseTime * Math.PI);
                    _slotClips[i].itemName._alpha = _baseAlpha[i] * pulse;
                }
                // Priority 2: Expiring slow pulse (2.5s cycle, 70-100% alpha)
                else if (_visualState[i] == STATE_EXPIRING) {
                    var expiringPulse:Number = 0.7 + 0.3 * Math.sin(_pulseTime * Math.PI * 2 / EXPIRING_CYCLE_SEC);
                    _slotClips[i].itemName._alpha = _baseAlpha[i] * expiringPulse;
                }
                // Priority 3: Confirmed single flash (decay from 100% to 50% over 0.5s)
                else if (_confirmTimer[i] > 0) {
                    var t:Number = 1 - (_confirmTimer[i] / CONFIRM_FLASH_SEC);
                    var flashDip:Number = 1 - 0.5 * Math.sin(Math.PI * t);
                    _slotClips[i].itemName._alpha = _baseAlpha[i] * flashDip;

                    _confirmTimer[i] -= dt;
                    if (_confirmTimer[i] < 0) _confirmTimer[i] = 0;
                }
                // Priority 4: Global refresh flash (legacy, for backward compat)
                else if (_flashTimer > 0) {
                    var ft:Number = 1 - (_flashTimer / FLASH_SEC);

                    if (_refreshMode == 1) {
                        // Pulse mode: alpha dip (strength controls dip depth)
                        var fdip:Number = 1 - _refreshStrength * Math.sin(Math.PI * ft);
                        _slotClips[i].itemName._alpha = _baseAlpha[i] * fdip;
                    }
                    else if (_refreshMode == 2) {
                        // Tint mode: color darkening (strength controls blend amount)
                        var tintAmount:Number = _refreshStrength * Math.sin(Math.PI * ft);
                        _slotClips[i].itemName.textColor = lerpColor(_baseColor[i], TINT_COLOR, tintAmount);
                    }
                    // mode 0 (none): do nothing
                }
                // No effect: base alpha
                else {
                    _slotClips[i].itemName._alpha = _baseAlpha[i];
                }
            }

        }

        // Decrement global flash timer
        if (_flashTimer > 0) {
            _flashTimer -= dt;
            if (_flashTimer < 0) _flashTimer = 0;
        }

        // Tint ended — restore base colors on all idle slots (no active visual effects)
        if (_flashTimer <= 0 && _tintDirty) {
            _tintDirty = false;
            for (var j:Number = 0; j < _activeSlotCount; j++) {
                var hasVisualEffect:Boolean = (_animPhase[j] != 0) ||
                                              (_confirmTimer[j] > 0) ||
                                              (_visualState[j] == STATE_OVERRIDE) ||
                                              (_visualState[j] == STATE_WILDCARD) ||
                                              (_visualState[j] == STATE_EXPIRING);
                if (!hasVisualEffect) {
                    _slotClips[j].itemName.textColor = _baseColor[j];
                }
            }
        }
    }

    // ===========================================================
    //  Content helpers
    // ===========================================================

    /** Update text content only — no alpha or position reset.
     *  Safe to call during active effects (flash, pulse, confirm). */
    private function updateDisplayText(index:Number, name:String,
                                        type:Number, detail:String):Void
    {
        var nf:TextField = _slotClips[index].itemName;
        var color:Number = getColorForType(type, index);
        _baseColor[index] = color;

        // Compose display text: "Name · detail", just "Name", or just detail
        if (detail.length > 0 && name.length > 0) {
            nf.text = name + " \u00B7 " + detail;
        } else if (detail.length > 0) {
            nf.text = detail;
        } else {
            nf.text = name;
        }

        nf.textColor = color;
        _nameFormat.color = color;
        nf.setTextFormat(_nameFormat);

        // Auto-resize background if text width changed
        var prevW:Number = _bgWidth;
        recalcWidth();
        if (_bgWidth != prevW) {
            drawBackground(_activeSlotCount);
        }
    }

    /** Apply content to a slot's item name field (no animation).
     *  Sets text AND resets alpha/position — only call when no effects active. */
    private function applyItemContent(index:Number, name:String,
                                       type:Number, confidence:Number,
                                       detail:String):Void
    {
        updateDisplayText(index, name, type, detail);
        var nf:TextField = _slotClips[index].itemName;
        nf._alpha = getAlphaForType(type);
        nf._y = 2;
        _baseAlpha[index] = getAlphaForType(type);
    }

    // ===========================================================
    //  Utility
    // ===========================================================

    private function resizeBackground():Void
    {
        recalcWidth();
        drawBackground(_activeSlotCount);
    }

    /** Scan visible slot names and resize _bgWidth to fit the widest. */
    private function recalcWidth():Void
    {
        var maxTW:Number = 0;
        for (var i:Number = 0; i < _activeSlotCount; i++) {
            var nf:TextField = _slotClips[i].itemName;
            if (nf.text.length > 0 && nf.textWidth > maxTW) {
                maxTW = nf.textWidth;
            }
        }
        // Width = left padding + key label + gap + text + right padding + gutter
        var needed:Number = NAME_X + maxTW + PADDING * 2 + 8;
        // Floor: never narrower than the default SLOT_WIDTH
        if (needed < SLOT_WIDTH + PADDING * 2) {
            needed = SLOT_WIDTH + PADDING * 2;
        }
        _bgWidth = needed;
    }

    /** Linearly interpolate between two RGB colors. t=0 returns c1, t=1 returns c2. */
    private function lerpColor(c1:Number, c2:Number, t:Number):Number
    {
        var r1:Number = (c1 >> 16) & 0xFF;
        var g1:Number = (c1 >> 8) & 0xFF;
        var b1:Number = c1 & 0xFF;
        var r2:Number = (c2 >> 16) & 0xFF;
        var g2:Number = (c2 >> 8) & 0xFF;
        var b2:Number = c2 & 0xFF;
        var r:Number = r1 + (r2 - r1) * t;
        var g:Number = g1 + (g2 - g1) * t;
        var b:Number = b1 + (b2 - b1) * t;
        return (Math.round(r) << 16) | (Math.round(g) << 8) | Math.round(b);
    }

    private function getColorForType(type:Number, index:Number):Number
    {
        if (type == TYPE_SPELL)          return (index == 0) ? COLOR_SPELL_TOP : COLOR_SPELL;
        if (type == TYPE_WILDCARD)       return COLOR_WILDCARD;
        if (type == TYPE_HEALTH_POTION)  return COLOR_HEALTH_POT;
        if (type == TYPE_MAGICKA_POTION) return COLOR_MAGICKA_POT;
        if (type == TYPE_STAMINA_POTION) return COLOR_STAMINA_POT;
        if (type == TYPE_MELEE_WEAPON)   return COLOR_WEAPON;
        if (type == TYPE_RANGED_WEAPON)  return COLOR_WEAPON;
        return COLOR_EMPTY;
    }

    private function getAlphaForType(type:Number):Number
    {
        if (type == TYPE_NOMATCH) return 50;
        if (type == TYPE_EMPTY)   return 50;
        return 100;
    }
}
