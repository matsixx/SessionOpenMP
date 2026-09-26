"""Board stance: two things that must hold before a build goes to the field.

1. THE ROTATION FEEDBACK. UpdateFootAnchors smooths the foot socket rotation FROM its previous value:
       q = Slerp(socket_prev, target, alpha)
   so a rotation of ours left in the socket is fed back and compounds. Simulated here with the game's
   own shape: without taking ours out first it settles near theta/alpha; with Stance_PreAnchors'
   unwind it is exactly theta on top of what the game would have had anyway.

2. THE DECK MODEL. The kick is a ramp from the truck through the idle foot's home. Moving a foot must
   give: nothing at zero offset; a rotation about the ACROSS axis only (never a roll about the board's
   length -- the bug this module has already shipped once); and, taken all the way from home on the
   tail to the flat, exactly the kick's tilt and exactly its rise.
"""
import math

# ---------------------------------------------------------------- quaternion helpers (x, y, z, w)
def qaxis(ax, deg):
    n = math.sqrt(sum(c * c for c in ax)); h = math.radians(deg) * 0.5; s = math.sin(h) / n
    return [ax[0] * s, ax[1] * s, ax[2] * s, math.cos(h)]
def qmul(a, b):
    return [a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1],
            a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0],
            a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3],
            a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2]]
def qinv(q): return [-q[0], -q[1], -q[2], q[3]]
def qnorm(q):
    n = math.sqrt(sum(c * c for c in q)); return [c / n for c in q]
def slerp(a, b, t):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0: b = [-c for c in b]; d = -d
    if d > 0.9995: return qnorm([x + (y - x) * t for x, y in zip(a, b)])
    th = math.acos(d); s = math.sin(th)
    return [(math.sin((1 - t) * th) * x + math.sin(t * th) * y) / s for x, y in zip(a, b)]
def angle(q):                       # rotation angle of a unit quaternion, degrees
    return math.degrees(2 * math.acos(min(1.0, abs(q[3]))))
def rot(q, v):
    p = qmul(qmul(q, v + [0.0]), qinv(q)); return p[:3]

# ---------------------------------------------------------------- 1. the feedback
target, ours, alpha = qaxis([0, 0, 1], 30.0), qaxis([0, 1, 0], 5.0), 0.12
worst_plain = worst_fixed = 0.0
for fixed in (False, True):
    game = target[:]; socket = target[:]
    for frame in range(600):
        if fixed:                               # Stance_PreAnchors: hand the game its own value back
            socket = qmul(qinv(ours), socket)
        game = slerp(socket, target, alpha)     # UpdateFootAnchors' smoothing, from the socket
        socket = qmul(ours, game)               # our rotation composed on after it
    extra = angle(qmul(socket, qinv(target)))   # how far the drawn foot is from the game's own
    if fixed: worst_fixed = abs(extra - 5.0)
    else:     worst_plain = extra
print("feedback, no unwind : our 5 deg became %.1f deg (theta/alpha = %.1f)" % (worst_plain, 5.0 / alpha))
print("feedback, unwound   : our 5 deg is %.3f deg off 5" % worst_fixed)

# ---------------------------------------------------------------- 2. the deck model (flipper frame)
truck = [21.3, 21.3]                  # |x| of the trucks, -x side / +x side (measured, 3.19.410 log)
homes = [(-31.0, 18.9), (19.0, 14.7)] # (x, h) of the idle feet: back foot ON the tail, front on the flat
zs = 1.0

def kick_from():
    lo = 0 if homes[0][1] <= homes[1][1] else 1; hi = 1 - lo
    rise = homes[hi][1] - homes[lo][1]
    run = abs(homes[hi][0]) - truck[1 if homes[hi][0] >= 0 else 0]
    return dict(hflat=homes[lo][1], rise=rise, run=run, tilt=math.atan2(rise, run))
K = kick_from()

def deck_at(x):
    over = abs(x) - truck[1 if x >= 0 else 0]
    if over <= 0: return K['hflat'], 0.0
    u = min(max(over / K['run'], 0.0), 1.8)
    return K['hflat'] + K['rise'] * u, K['tilt'] * min(u, 1.0)

def normal(x, t):
    s = 1.0 if x >= 0 else -1.0
    return [-s * math.sin(t), 0.0, zs * math.cos(t)]

def rot_between(a, b):
    c = [a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]]
    d = sum(x * y for x, y in zip(a, b))
    return qnorm(c + [1.0 + d])

worst_roll = worst_zero = 0.0
for x0 in [-40 + i * 0.5 for i in range(161)]:
    for dx in [-25 + j for j in range(51)]:
        h0, t0 = deck_at(x0); h1, t1 = deck_at(x0 + dx)
        q = rot_between(normal(x0, t0), normal(x0 + dx, t1))
        # the axis must be the ACROSS axis (flipper Y): any X component is a roll about the length
        worst_roll = max(worst_roll, abs(q[0]), abs(q[2]))
        if dx == 0: worst_zero = max(worst_zero, abs(h1 - h0), angle(q))

x_home = homes[0][0]
h0, t0 = deck_at(x_home); h1, t1 = deck_at(-truck[0] + 3.0)      # from the tail home onto the flat
q = rot_between(normal(x_home, t0), normal(-truck[0] + 3.0, t1))
print("deck: kick %.1f cm over %.1f cm = %.1f deg" % (K['rise'], K['run'], math.degrees(K['tilt'])))
print("deck: worst roll component %.2e, worst change at zero offset %.2e" % (worst_roll, worst_zero))
print("deck: tail -> flat drops %.2f cm (kick rise %.2f) and turns %.2f deg (kick %.2f)"
      % (h0 - h1, K['rise'], angle(q), math.degrees(K['tilt'])))

ok = (worst_plain > 20.0 and worst_fixed < 1e-3 and worst_roll < 1e-9 and worst_zero < 1e-9
      and abs((h0 - h1) - K['rise']) < 1e-6 and abs(angle(q) - math.degrees(K['tilt'])) < 1e-3)

# ---------------------------------------------------------------- 3. the angle signs, from geometry
# 3.19.415 stopped measuring the yaw/pitch signs per frame (the test flickered) and derives them:
# toes along side*up x lenX; yaw about `up` by -side*travel swings them toward the nose (+along);
# pitch about `along` by +side*travel lifts them. Checked with the mod's own quaternion formulas
# (TwkQuatRotate / TwkQuatAxisAngle), random frames, both sides, both directions of travel, and toes
# angled up to 40 degrees off square.
import random
def twk_rotate(q, v):
    vx, vy, vz = v
    tx = 2 * (q[1]*vz - q[2]*vy); ty = 2 * (q[2]*vx - q[0]*vz); tz = 2 * (q[0]*vy - q[1]*vx)
    return [vx + q[3]*tx + (q[1]*tz - q[2]*ty), vy + q[3]*ty + (q[2]*tx - q[0]*tz), vz + q[3]*tz + (q[0]*ty - q[1]*tx)]
def twk_axis(ax, deg):
    n = math.sqrt(sum(c*c for c in ax)); h = deg * 0.008726646; s_ = math.sin(h) / n
    return [ax[0]*s_, ax[1]*s_, ax[2]*s_, math.cos(h)]
def cross(a, b): return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]
def dot(a, b): return sum(x*y for x, y in zip(a, b))
def unit(v):
    n = math.sqrt(dot(v, v)); return [c/n for c in v]
random.seed(5)
bad_yaw = bad_pitch = 0; trials = 0
for _ in range(20000):
    up = unit([random.uniform(-1, 1) for _ in range(3)])
    lx = [random.uniform(-1, 1) for _ in range(3)]
    lx = unit([a - dot(lx, up)*b for a, b in zip(lx, up)])
    side0 = cross(up, lx)
    side = random.choice([1, -1]); travel = random.choice([1, -1])
    along = [c * travel for c in lx]
    ang = math.radians(random.uniform(-40, 40))            # toes angled off square
    toe = unit([side * (math.cos(ang)*s0) + math.sin(ang)*l for s0, l in zip(side0, lx)])
    y = twk_rotate(twk_axis(up, 3.0 * (-side * travel)), toe)
    p_ = twk_rotate(twk_axis(along, 3.0 * (side * travel)), toe)
    trials += 1
    if dot(y, along) <= dot(toe, along): bad_yaw += 1
    if dot(p_, up) <= dot(toe, up): bad_pitch += 1
print("signs: + yaw failed to swing the toes toward the nose %d / %d; + pitch failed to lift them %d / %d"
      % (bad_yaw, trials, bad_pitch, trials))
ok = ok and bad_yaw == 0 and bad_pitch == 0

# ---------------------------------------------------------------- 4. the deck must not flicker
# 3.19.420: the kick was re-derived every frame from the two homes and only counted past 1.0 cm of
# rise. Replayed with the front foot's logged home heights (it bobs 13.3 -> 14.5 through the idle)
# and the back foot at 15.1: the old model switches the kick -- and with it the back foot's drop --
# on and off; the held model gives one drop, always.
front_h = [13.3, 13.5, 13.6, 14.0, 14.0, 14.1, 14.5, 14.3, 13.9, 13.4, 13.6, 14.2]
back_h, run = 15.1, 9.0
def drop_old(fh):
    rise = back_h - fh
    return rise if rise >= 1.0 else 0.0          # the back foot's drop coming off the kick, cm
held = sum(back_h - fh for fh in front_h) / len(front_h)
old = [drop_old(fh) for fh in front_h]
toggles = sum(1 for a, b in zip(old, old[1:]) if (a > 0) != (b > 0))
print("deck: old per-frame kick switched on/off %d times over %d samples (drops %s cm)"
      % (toggles, len(old), " ".join("%.1f" % d for d in old)))
print("deck: held kick drops %.2f cm every time" % held)
ok = ok and toggles > 0 and held > 0.5

# ---------------------------------------------------------------- 5. a board that comes back reversed
# 3.19.421: positions were stored in the BOARD's frame. The 420 log, after a trick that rotated the
# board 180: homes L +10.2 / R -29.7, feet read L -10.1 / R +29.7 -- 20 and 59 cm "from home", so the
# stance stepped aside and re-learned 3 s later. In the travel frame (board x * travel, travel = which
# way the board's own +x points relative to the heading, and it flips with the board) nothing moved.
home_l, home_r = 10.2, -29.7
before = (10.2, -29.7, +1)                 # (L, R in the board frame, travel)
after  = (-10.1, 29.7, -1)                 # reversed board: its +x now points backwards
def far(pos, travel, use_travel):
    l, r = (pos[0] * travel, pos[1] * travel) if use_travel else (pos[0], pos[1])
    return abs(l - home_l), abs(r - home_r)
print("reversal, board frame : feet %.1f / %.1f cm from home" % far(after[:2], after[2], False))
print("reversal, travel frame: feet %.1f / %.1f cm from home" % far(after[:2], after[2], True))
ok = ok and max(far(after[:2], after[2], True)) < 0.5 and max(far(after[:2], after[2], False)) > 10

# ---------------------------------------------------------------- 6. the foot goes to a SPOT
# 3.19.425: after a 360 flip the game's idle parks the front foot at 15.5 cm instead of 10.0. With the
# rider's front-foot offset of -58 mm, the old model (an amount added to the animation, backed off to
# ~73% by the distance) landed the foot at about 11.3 -- right back on the default spot. The new model
# pins it to home + offset whatever the animation did.
home, off, anim_normal, anim_after_360 = 10.0, -5.8, 10.0, 15.5
def old(anim):
    reach = max(8.0, abs(off) + 4.0); w = max(0.0, min(1.0, 1.0 - (abs(anim - home) - 4.0) / (reach - 4.0)))
    return anim + off * w
def new(anim): return anim + (home + off - anim) * 1.0
for anim in (anim_normal, anim_after_360):
    print("front foot, game idle at %.1f: old model %.1f cm, new %.1f cm (wanted %.1f)" % (anim, old(anim), new(anim), home + off))
ok = ok and abs(new(anim_after_360) - (home + off)) < 1e-6 and abs(old(anim_after_360) - home) < 1.5

# ---------------------------------------------------------------- 7. front from the stance, no heading
# 3.19.427: which foot leads = goofy XOR switch (right leads if so); the board's front end = wherever
# that foot is. Replayed with logged board-frame positions: the main stance (L +10 / R -30), switch (R
# leads, L on the kick at 26), and both again with the board reversed by a shuvit (every x negated).
def frame(fx_l, fx_r, goofy, switch):
    front_right = goofy != switch
    ff, fb = (fx_r, fx_l) if front_right else (fx_l, fx_r)
    travel = 1.0 if ff - fb >= 0 else -1.0
    return front_right, fx_l * travel, fx_r * travel
cases = [("main",   10.0, -30.2, False, False, (10.0, -30.2)),
         ("switch", -26.1, 12.8, False, True,  (-26.1, 12.8))]
bad = 0
for name, l, r, g, sw, want in cases:
    for rev in (1.0, -1.0):                      # as ridden, and with the board spun round
        fr, al, ar = frame(l * rev, r * rev, g, sw)
        okc = abs(al - want[0]) < 1e-6 and abs(ar - want[1]) < 1e-6
        bad += 0 if okc else 1
        print("%-6s board %s: front=%s, along L %+.1f / R %+.1f %s" % (name, "reversed" if rev < 0 else "as ridden",
              "R" if fr else "L", al, ar, "" if okc else "  <-- WRONG"))
ok = ok and bad == 0

# ---------------------------------------------------------------- 8. the pieces are still there
# 3.19.427 shipped with the home-learning step deleted by an edit aimed at the code beside it: the
# stance waited forever to learn the rider's feet and never applied. Nothing about that is visible
# until someone rides, so the source is checked for the steps the stance cannot run without.
import os
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "tweaks", "board_stance.cpp"), encoding="utf-8").read()
need = {
    "learns the idle homes":              "learned the normal idle",
    "leaves when the frame is unreadable": "if (!feet) { g_shown = 0.0f;",
    "takes its rotation out first":       "void Stance_PreAnchors(",
    "measures the deck once":             "deck measured for stance",
    "names a held blocker":               "held off for 5 s on the ground",
    "keeps the idle for the session":     "goofy/regular changed: learning your idle afresh",
    "keeps the idle across launches":     "StanceHome%d%cX",
    "saves a first learn":                "if (learned) TwkMarkDirty();",
    "holds the landing against the clip": "TwkQuatMul(q1, qHold, q1h);",
    "follows from the animated spot":     "DeckNormal(rd, x0, t0, n0);",
    "tells getting on from a landing":    "s_prevMode == BM_FALLING && mode == BM_SKATEBOARDING",
    "starts the landing clock":           ": the feet go over the bolts for %d ms",
    "never learns from a landing":        "(block || landFlag || landActive || s_landSt) ? 0.0f",
}
missing = [k for k, v in need.items() if v not in src]
print("source: " + ("every required step present" if not missing else "MISSING: " + ", ".join(missing)))
ok = ok and not missing
# 505: the deck is measured BEFORE a stance left at zero leaves -- else a stock stance never measures it and
# its landings wait on "measuring the deck" forever.
i_deck, i_zero = src.find("deck measured for stance"), src.find("every slider for this stance is at zero")
order_ok = 0 <= i_deck < i_zero
print("source: the deck is measured " + ("before a zero stance leaves" if order_ok else "AFTER a zero stance leaves  <-- WRONG"))
ok = ok and order_ok
# ---------------------------------------------------------------- 9. stepping off the landing
# 3.19.437: after a landing the feet SWIVEL from the bolts to the stance (ball pivot, heel pivot).
# Replicated from board_stance.cpp (Kin / LatchStep / StepPose) and checked: the step starts on the
# landing pose and ends exactly on the stance, never jumps between 120 Hz frames, keeps the ball
# planted through a ball pivot and the heel planted through a heel pivot, and lifts the heel ~1 cm.
BALL, MAXSW, DEG_PER_CM, HEEL_UP, TOE_UP, PIVOT = 14.0, 24.0, 2.4, 3.0, 2.0, 0.14
DEG = math.pi / 180.0
def minjerk(u):
    u = min(max(u, 0.0), 1.0); return u * u * u * (10 - 15 * u + 6 * u * u)
def kin(L, st, tau):
    hx = hy = psi = pitch = rise = 0.0
    for j in range(2 * st['n']):
        u0 = (tau - st['t0'][j]) / st['td'][j]
        if u0 <= 0: break
        u = min(u0, 1.0); m = minjerk(u); ball = (j % 2 == 0)
        dpsi = (-st['dir'] if ball else st['dir']) * st['phi']
        if ball:
            b0 = (L[2] + psi) * DEG; b1 = (L[2] + psi + dpsi * m) * DEG
            bx = hx + BALL * math.sin(b0); by = hy + BALL * math.cos(b0)
            hx = bx - BALL * math.sin(b1); hy = by - BALL * math.cos(b1)
        psi += dpsi * m
        if u0 < 1:
            bump = math.sin(math.pi * u)
            if ball: pitch -= HEEL_UP * bump; rise += BALL * math.sin(HEEL_UP * bump * DEG)
            else: pitch += TOE_UP * bump
    return hx, hy, psi, pitch, rise
def latch(L, S):
    dx = S[0] - L[0]; d = abs(dx)
    st = dict(dir=1.0 if dx >= 0 else -1.0)
    st['n'] = 0 if d < 1.5 else (1 if d <= 12.0 else 2)
    per = d / st['n'] if st['n'] else 0.0
    st['phi'] = min(MAXSW, DEG_PER_CM * per, math.asin(min(1.0, per / BALL)) / DEG) if st['n'] else 0.0
    sw = 2 * PIVOT * 0.84 * min(max(0.8 + 0.03 * per, 0.85), 1.2)
    st['t0'] = []; st['td'] = []
    for i in range(st['n']):
        base = i * sw * 0.9
        st['t0'] += [base, base + sw * 0.42]; st['td'] += [sw * 0.58, sw * 0.58]
    st['T'] = (st['n'] - 1) * sw * 0.9 + sw if st['n'] else 0.22
    ex, ey, _, _, _ = kin(L, st, st['T'] + 1.0); st['ex'] = ex; st['ey'] = ey
    return st
def pose(L, S, st, tau):
    if tau >= st['T']: return list(S) + [0.0]
    hx, hy, psi, pitch, rise = kin(L, st, tau); m = minjerk(tau / st['T'])
    return [L[0] + hx + (S[0] - L[0] - st['ex']) * m, L[1] + hy + (S[1] - L[1] - st['ey']) * m,
            L[2] + psi + (S[2] - L[2]) * m, L[3] + pitch + (S[3] - L[3]) * m, rise]
worst_end = worst_jump = worst_plant = worst_rise = 0.0
cases = [((-21.3, 0.0, 0.0, 0.0), (-31.0, 0.0, 0.0, 0.0)),     # back foot: bolts -> tail
         ((21.3, 0.0, 0.0, 0.0), (12.0, 0.5, -5.0, 0.0)),      # front foot: bolts -> flat, angled
         ((-18.3, 1.5, 6.0, 0.0), (-33.0, -1.0, 8.0, 5.0)),    # a varied landing onto a far stance: 2 swivels
         ((21.3, 0.0, 0.0, 0.0), (12.7, 0.0, 0.0, 0.0)),       # the 444 log's typical front foot: 8.6 cm, 1 swivel
         ((20.0, 0.0, 0.0, 0.0), (20.8, 0.0, 0.0, 0.0))]       # too close to swivel: a settle
for L, S in cases:
    st = latch(L, S)
    p0 = pose(L, S, st, 0.0)
    worst_end = max(worst_end, max(abs(a - b) for a, b in zip(p0[:4], L)))
    pT = pose(L, S, st, st['T'] - 1e-6)
    worst_end = max(worst_end, max(abs(a - b) for a, b in zip(pT[:4], S)), abs(pT[4]))
    prev = p0; t = 0.0
    while t < st['T'] + 0.05:
        t += 1.0 / 120.0
        q = pose(L, S, st, t)
        worst_jump = max(worst_jump, abs(q[0] - prev[0]), abs(q[1] - prev[1]), abs(q[2] - prev[2]) * 0.1)
        worst_rise = max(worst_rise, q[4]); prev = q
    # the pure swivels: ball fixed through a ball pivot, heel fixed through a heel pivot -- where that
    # pivot runs alone (pivots overlap at their ends)
    for j in range(2 * st['n']):
        a0 = max(st['t0'][j], st['t0'][j - 1] + st['td'][j - 1] if j else 0.0)
        a1 = min(st['t0'][j] + st['td'][j], st['t0'][j + 1] if j + 1 < 2 * st['n'] else 1e9)
        pts = []
        for k in range(11):
            h = kin(L, st, a0 + (a1 - a0) * k / 10.0)
            yaw = (L[2] + h[2]) * DEG
            pts.append((h[0] + BALL * math.sin(yaw), h[1] + BALL * math.cos(yaw)) if j % 2 == 0 else (h[0], h[1]))
        worst_plant = max(worst_plant, max(math.hypot(p_[0] - pts[0][0], p_[1] - pts[0][1]) for p_ in pts))
print("step: worst start/end miss %.2e, worst per-frame move %.2f cm, worst planted-point slip %.2e cm, heel rise %.2f cm"
      % (worst_end, worst_jump, worst_plant, worst_rise))
swivels = [latch(L, S)['n'] for L, S in cases]
print("step: swivels per case %s (8.6 cm must be ONE)" % swivels)
ok = ok and worst_end < 1e-3 and worst_jump < 1.5 and worst_plant < 1e-6 and 0.5 < worst_rise < 1.5 and swivels[3] == 1

print("PASS" if ok else "FAIL")
