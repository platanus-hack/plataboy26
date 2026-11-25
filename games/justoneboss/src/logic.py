"""Just One Boss: the game logic, compiled to bytecode by tools/jbc.py (see there for
the language: a small subset of Python). The C side (game.c) keeps the entities,
moves, particles, flowers, hits and the drawing primitives; this file is the cart's
Lua: entity classes (update, draw, hurt, death), the player, the boss's choreography
(scripts: functions that wait(), run as threads) and the screens.

Entities are indices into E (NONE = no entity: its fields read as 0, writes are
dropped); positions are in 1/16 pixel (FX), the stage is x 0..80, y 0..40."""

NONE = 255
NE = 29
# entity classes (the cart's numbers)
T_TOPHAT = 1
T_SPINHAT = 2
T_BUNNY = 3
T_TITLE = 6
T_CREDIT = 7
T_VICTORY = 8
T_GAMEOVER = 9
T_FIGMENT = 10
T_PLAYER = 11
T_PHEALTH = 12
T_TILESPAWN = 14
T_TILE = 15
T_PREFL = 16
T_CARD = 17
T_COIN = 19
T_SLAM = 20
T_MIRROR = 22
T_MREFL = 23
T_HAND = 24
T_LASER = 25
T_HEART = 26
T_POINTS = 29
# e.fl
F_DEAD = 1
F_VIS = 2
F_RVIS = 4
F_IDLE = 8
F_REFL = 16
F_RIGHT = 32
F_OCC = 64
F_BOSS = 128
# e.fl2: mirror (bits 0-2 reflection colour, 5-6 home_x / 20), hand
M_HAT = 8
M_CRACK = 16
H_WAND = 1
H_BOUQ = 2
# easing, effects, sprite flags, buttons
LIN = 0
EI = 1
EO = 2
EOI = 3
X_POOF = 1
X_PAIN = 2
G_FLIPX = 1
G_DITHER = 4
BTN_A = 16
BTN_B = 32
HOME_Y = FX(-28)


# ===================================================================================
# helpers
# ===================================================================================
CONJ = [1, 2, 3, 5, 7, 9, 10, 11]
SIN64 = [0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 126, 127]

def isin(a):                                 # sin * 127, 256 = one turn
    q = (a >> 2) & 15
    v = SIN64[16 - q] if a & 64 else SIN64[q]
    if (a & 127) == 64:
        v = 127
    return -v if a & 128 else v
# per class: layer | pause immune 32 | boss generated 64, frames to death, hitbox | hurtbox << 4
CLS_L = [0, 5, 73, 5, 0, 0, 50, 50, 50, 50, 49, 5, 45, 0, 5, 67, 5, 75, 0, 69, 4, 0, 7, 69, 5, 74, 5, 0, 0, 10]
CLS_T = [0, 0, 120, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10, 0, 0, 100, 0, 0, 7, 0, 0, 0, 0, 16, 150, 0, 0, 32]
CLS_H = [0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 18, 0, 0, 0, 32, 18, 1, 0, 0, 1, 0, 0, 0, 0, 1, 32, 0, 0, 0]

@cold
def spawn(t, x, y):
    for k in range(NE):
        e = G.spawn_n
        G.spawn_n = e + 1 if e < NE - 1 else 0
        if not e.type:
            ent_new(e)                       # all fields 0, in the update/draw order
            e.type = t
            e.x = x
            e.y = y
            k = CLS_L[t]
            e.layer = k & 31
            e.ftd = CLS_T[t]
            e.hit = CLS_H[t]
            if k & 64:
                e.fl = F_BOSS
            return e
    return NONE

@cold
def poof(e, dx, dy, snd):
    sfx(snd if snd else 11, 2)
    effect(X_POOF, e.x + FX(dx), e.y + FX(dy))
    return 12

@cold
def move(e, x, y, d, es):                    # default anchors: (end - start) / 4
    dx = (x - e.x) >> 6
    dy = (y - e.y) >> 6
    return move4(e, x, y, d, es, dx, dy, -dx, -dy)

@cold
def burst(e, dy, n, speed, health):          # spawn_particle_burst; the first `health`
    got = 0                                  # particles fly to the boss bar
    for i in range(1, n + 1):
        a = (i * 256 + rnd(179)) // n
        s = speed * (32 + rnd(45)) // 4      # speed * (0.5 + rnd 0.7), 1/16 pixel
        k = 1
        t = 13 + rnd(7)
        if got < health:
            k = 3 | got << 3
            t = 9 + 2 * got
        if particle(e.x, e.y - FX(dy), s * isin(a + 64) // 32, -s * isin(a) // 32 - speed * 32, t, k) and k > 1:
            got += 1
    G.owed += health - got                   # no room: they still reach the bar (game.c)

@cold
def keep_only(a, b, c):                      # the cart's entities = {title, curtains, ...}
    for e in range(NE):
        if e.type and e != G.title and e != a and e != b and e != c:
            e.fl |= F_DEAD
    kill_threads()

# flower fields (game.c draws them, counts dc / ftd down, hurts): FL bytes, 14 a field:
# own, dc, ftd, on, mask[5] (shown), pend[5] (planned)
@cold
def field_new(m):
    for f in range(3):
        b = f * 14
        if not peek(b + 3):
            for i in range(14):
                poke(b + i, 0)
            poke(b, m)
            poke(b + 3, 1)
            return f
    return NONE

@cold
def field_bit(f, b, k):                      # k = 4: shown, 9: planned
    return peek(f * 14 + k + (b >> 3)) >> (b & 7) & 1

@cold
def field_set(f, b, k, v):
    i = f * 14 + k + (b >> 3)
    m = 1 << (b & 7)
    poke(i, (peek(i) | m) if v else (peek(i) & ~m))

@cold
def field_show(f, b):                        # a planned flower appears
    if f == NONE or not field_bit(f, b, 9):
        return 0
    field_set(f, b, 9, 0)
    field_set(f, b, 4, 1)
    return 1

@cold
def fields_off():
    for f in range(3):
        poke(f * 14 + 3, 0)

@cold
def reveal(m, f):                            # one more flower per frame, scattered
    s = rnd(40)
    for i in range(40):
        if field_show(f, (s + 7 * i) % 40):
            sfx(15, 1)
            wait(1)

@cold
def flowers(m, refl):                        # the flower locations (do_a_math)
    f = field_new(m)
    if f == NONE:
        return 0
    fork(m, reveal, f, 1)
    n = 0
    k = CONJ[G.conjure - 1]
    for i in range(40):
        if i == n:
            s = ((n + k) % 2 > 0) + ((n + k) % 3 > 0) + ((n + k) % 5 > 0)
            n += s if s else 1
            if not refl:
                field_set(f, i, 9, 1)
        elif refl:
            field_set(f, i, 9, 1)

@cold
def bloom_field(m):
    for f in range(3):
        i = f * 14
        if peek(i + 3) and peek(i) == m and not peek(i + 2):
            poke(i + 1, 4)                   # dc: hurts for 4 frames
            poke(i + 2, 15)                  # ftd
            for b in range(9, 14):
                poke(i + b, 0)
            for b in range(40):
                if field_bit(f, b, 4):
                    x = FX(b % 8 * 10 + 5)
                    y = FX(b // 8 * 8 + 2)
                    particle(x, y, -32, -64 - rnd(64), 10 + rnd(7), 2)
                    particle(x, y, 32, -64 - rnd(64), 10 + rnd(7), 2)

def occupant(w):
    c = col(w.x)
    r = row(w.y)
    for e in range(NE):
        if e.type and e.fl & F_OCC and col(e.x) == c and row(e.y) == r:
            return e
    return NONE

def prompt(e, s, n, y):
    if e.alive % 30 < 22 and e.b:
        text(s, text("press > to ", (e.x >> 4) + G.shake_x - 2 * (11 + n), y, 1), y, 1)

def spr_w(e, dx, dy, i, fl):                 # mirrors and hands wobble (idle_step: u, v)
    spr(i, sx(e.x) + e.u - dx, sy(e.y + FX(e.v - dy)), fl)
@cold
def rnd_int(a, b):
    return a + rnd(b - a + 1)

@cold
def mid(a, b, c):
    if b < a:
        return a
    if b > c:
        return c
    return b

@cold
def hdir(h):
    return 1 if h.fl & F_RIGHT else -1

@cold
def spawn_magic_tile(ftd):
    if G.bh_health >= 60:
        G.bh_dc = 61
    e = spawn(T_TILESPAWN, FX(10) * rnd_int(1, 8) - FX(5), FX(8) * rnd_int(1, 5) - FX(4))
    e.ftd = ftd if ftd else 100


# ===================================================================================
# the mirror (boss), its hands, its reflections
# ===================================================================================
@cold
def spawn_mirror(t):
    m = spawn(t, FX(40), HOME_Y)
    m.fl |= F_RVIS
    m.c = 4                                  # expression
    m.fl2 = 2 << 5                           # home_x 40
    for r in range(2):
        h = spawn(T_HAND, m.x + (FX(18) if r else FX(-18)), m.y + FX(5))
        h.a = m
        h.b = 3                              # pose
        h.fl |= (F_RIGHT if r else 0) | (m.fl & F_BOSS)
        if r:
            m.b = h
        else:
            m.a = h
    return m

@cold
def spawn_mrefl():                           # class 23: a dithered copy of the boss
    ci = 3
    if G.hard:
        ci = G.refl_color
        G.refl_color = ci % 5 + 1
    r = spawn_mirror(T_MREFL)
    b = G.boss
    r.fl |= F_VIS | F_REFL
    r.fl2 = M_HAT | 1 << 5 | (ci - 1)        # hat on, home_x 20, colour
    r.x = b.x
    r.y = b.y
    r.c = b.c if b != NONE else 1
    for k in range(2):
        h = r.b if k else r.a
        bh = h
        if b != NONE:
            bh = b.b if k else b.a
        h.fl = (h.fl & ~F_VIS) | (bh.fl & F_VIS) | F_REFL
        h.b = bh.b
        h.x = bh.x
        h.y = bh.y
    return r

@cold
def spawn_reflection(dx, fn, arg):
    r = spawn_mrefl()
    move(r, r.x + FX(dx), r.y, 15, EI)
    move(r.a, r.a.x + FX(dx), r.a.y, 15, EI)
    move(r.b, r.b.x + FX(dx), r.b.y, 15, EI)
    fork(r, fn, arg, 15)
    sfx(30, 2)

@cold
def set_idle(m, v):
    for k in range(3):
        e = m.a if k == 0 else (m.b if k == 1 else m)
        if v:
            e.fl |= F_IDLE
        else:
            e.fl &= ~F_IDLE

@cold
def appear(e):
    if e.type == T_HAND:
        if e.fl & F_VIS:
            return 0
        e.fl |= F_VIS
        return poof(e, 0, 0, 0)
    e.fl |= F_RVIS
    appear(e.a)
    appear(e.b)
    return 0

@cold
def disappear(e):
    if e.type == T_HAND:
        if e.fl & F_VIS:
            e.fl &= ~F_VIS
            poof(e, 0, 0, 0)
        return 0
    e.fl &= ~F_RVIS
    disappear(e.a)
    disappear(e.b)

@cold
def ready(m, expr):                          # return_to_ready_position
    if m == NONE:
        return 0
    hx = FX(20) * ((m.fl2 >> 5) & 3)
    set_idle(m, 1)
    m.c = expr
    move(m, hx, HOME_Y, 15, EI)
    h = m.a
    h.fl2 &= ~H_WAND
    h.b = 3
    move4(h, hx - FX(18), HOME_Y + FX(5), 15, EI, -10, -10, -20, 0)
    appear(h)
    h = m.b
    h.fl2 &= ~H_WAND
    h.b = 3
    move4(h, hx + FX(18), HOME_Y + FX(5), 15, EI, 10, -10, 20, 0)
    appear(h)
    return 15 if G.hard else 25

@cold
def cancel_everything(m):
    for k in range(3):
        e = m.a if k == 0 else (m.b if k == 1 else m)
        if k < 2:
            e.fl2 &= ~H_WAND
        cancel_threads(e)
        cancel_move(e)
    m.dc = 0
    for e in range(NE):
        if e.type and e.fl & F_BOSS:
            cancel_threads(e)
            e.fl |= F_DEAD
    fields_off()

@cold
def despawn_coins(m):
    for e in range(NE):
        if e.type == T_COIN and e.a == m:
            die(e)
    return 10

@cold
def reform(r):
    b = G.boss
    if b != NONE:
        move(r, b.x, b.y, 10, EO)
        move(r.a, b.a.x, b.a.y, 10, EO)
        move(r.b, b.b.x, b.b.y, 10, EO)
    return 10


# ---- the boss's moves (the cart's methods; scripts run as threads: f(entity, arg)) ---
@cold
def hpound(h, arg):
    m = h.a
    d = hdir(h)
    h.b = 2
    wait(move4(h, m.x + FX(4) * d, m.y + FX(20), 15, EO, 20 * d, 0, 20 * d, 0))
    sfx(12, 2)
    shake(0, 2)
    wait(1)

@cold
def pound(m):
    fork(m.a, hpound, 0, 0)
    hpound(m.b, 0)

@cold
def temple(h, arg):                          # move_to_temple
    m = h.a
    h.b = 1
    wait(move(h, m.x + FX(13) * hdir(h), m.y, 20, LIN))

@cold
def reel_step(e):
    shake(0, 2)
    e.x = mid(FX(10), e.x, FX(70))
    e.y = mid(FX(-40), e.y, FX(-20))
    poof(e, rnd_int(-10, 10), rnd_int(-10, 10), 12)
    move(e, e.x + FX(rnd_int(-7, 7)), e.y + FX(rnd_int(-7, 7)), 6, EO)

@cold
def reel(m, times):
    spawn(T_HEART, FX(10) * rnd_int(3, 6) - FX(5), FX(4))
    appear(m.a)
    m.a.b = 3
    appear(m.b)
    m.b.b = 3
    if G.phase >= 3:
        m.fl2 |= M_CRACK
    m.c = 8
    set_idle(m, 0)
    for i in range(times):
        reel_step(m.a)
        reel_step(m.b)
        reel_step(m)
        wait(5)

@cold
def throw_hat(m, arg):
    h = m.b
    set_idle(m, 0)
    disappear(m.a)
    wait(appear(h))
    wait(move(h, m.x + FX(5), m.y - FX(6), 15, LIN))
    h.b = 1
    wait(30)
    h.b = 3
    sfx(26, 2)
    m.fl2 &= ~M_HAT
    s = spawn(T_SPINHAT, m.x, FX(-32))
    s.a = m
    wait(move(h, h.x + FX(14), h.y + FX(5), 3, EI))
    wait(30)

@cold
def conjure(m, extra):                       # conjure_flowers
    if G.hard or not m.fl & F_REFL:
        G.conjure = 1 + (G.conjure + rnd_int(0, 2)) % 8
    fork(m.a, temple, 0, 0)
    set_idle(m, 0)
    temple(m.b, 0)
    m.c = 2
    flowers(m, m.fl & F_REFL)
    wait(extra + (50 if G.hard else 65))
    sfx(16, 1)
    bloom_field(m)
    m.a.b = 5
    m.b.b = 5
    m.c = 3
    wait(31)

@cold
def flourish(h, arg):                        # flourish_wand
    d = hdir(h)
    wait(move4(h, FX(40) + FX(20) * d, FX(-30), 12, EO, -20, 20, 0, 20))
    h.b = 6
    sfx(23, 1)
    burst(h, 20, 20, 3, 0)
    shake(0, 20)

@cold
def rh_wand(h, arg):
    h.b = 1
    h.fl2 |= H_WAND
    wait(poof(h, -10, 0, 0))
    wait(30)
    flourish(h, 0)

@cold
def cast(m, upgraded):                       # cast_reflection
    l = m.a
    r = m.b
    set_idle(m, 0)
    wait(move(l, l.x + FX(23), l.y + FX(14), 20, EI))
    l.b = 1
    for i in range(2):
        wait(move4(r, r.x, r.y, 40, LIN, 18, 6, -18, 6))
    if upgraded:
        fork(r, rh_wand, 0, 0)
    m.c = 1
    l.fl2 |= H_WAND
    wait(poof(l, 10, 0, 0))
    wait(30)
    flourish(l, 0)
    m.c = 3
    wait(5)
    if upgraded:
        G.brefl = spawn_mrefl()
        m.fl2 += 1 << 5                      # home_x + 20
    else:
        p = G.player
        r = spawn(T_PREFL, FX(80) - p.x, p.y)
        G.prefl = r
        poof(r, 0, 0, 0)
    wait(55)

@cold
def hthrow(h, arg):                          # the hand's throw_cards
    d = hdir(h)
    first = (h.fl & F_RIGHT != 0) != (h.fl & F_REFL != 0)
    if not first:
        wait(13 if G.hard else 19)
    h.fl &= ~F_IDLE
    r = 0 if first else 1
    while r <= 4:
        h.b = 3
        wait(move4(h, FX(40) + FX(52) * d, FX(8) * r + FX(4), 18, EOI, 10 * d, -10, 10 * d, 10))
        h.b = 2
        if not G.hard:
            wait(12)
        h.b = 1
        sfx(13, 2)
        k = spawn(T_CARD, h.x - FX(7) * d, h.y)
        k.u = -24 * d
        wait(10)
        r += 2

@cold
def throw_cards(m, arg):
    fork(m.a, hthrow, 0, 0)
    hthrow(m.b, 0)

@cold
def coins(m, n, at_refl):                    # throw_coins
    r = m.b
    set_idle(m, 0)
    temple(r, 0)
    for i in range(n):
        m.c = 7
        r.b = 1
        wait(7 if G.hard else 15)
        tg = G.prefl if at_refl and G.prefl != NONE else G.player
        tx = 10 * col(tg.x) - 5
        ty = 8 * row(tg.y) - 4
        sfx(21, 1)
        k = spawn(T_COIN, m.x + FX(13), m.y - FX(6))
        k.a = m
        k.b = tx
        k.c = ty
        k.idle = 3                           # health
        move4(k, FX(tx + 2), FX(ty), 25, EO, 20, -30, 10, -60)
        r.b = 4
        m.c = 3
        wait(20)

@cold
def shoot_laser(m, lng):
    wait(2 if G.hard else 12)
    sfx(7 if lng else 14, 1)
    m.dc = 173 if lng else 31
    wait(12)
    m.c = 0
    shake(0, 4)
    l = spawn(T_LASER, m.x, m.y)
    l.a = m
    if lng:
        l.ftd = 150
    wait(166 if lng else 16)
    m.c = 5
    wait(5)

@cold
def refl_lasers(r, n):
    for i in range(n):
        wait(10)
        shoot_laser(r, 0)
    die(r)

@cold
def shoot_lasers(m, sweep):
    disappear(m.a)
    disappear(m.b)
    c = rnd_int(0, 7)
    n = 2
    m.c = 5
    set_idle(m, 0)
    for i in range(3):
        c = (c + rnd_int(2, 3 if G.hard else 6)) % 8
        wait(move4(m, FX(10) * c + FX(5), FX(-20), 10 if G.hard else 15, EI, 0, -10, 0, -10))
        wait(1)
        if sweep:
            d = 2
            if c > 5 or (rnd(2) and c > 1):
                d = -2
            c += d
            move(m, m.x + FX(10) * d, m.y, 40, LIN)
        shoot_laser(m, 0)
        if G.hard and G.phase > 1 and n > 0:
            fork(spawn_mrefl(), refl_lasers, n, 0)
            n -= 1


# ---- decide_next_action -----------------------------------------------------------------
@cold
def p1(m):
    wait(ready(m, 1))
    wait(15)
    hthrow(m.a, 0)
    wait(ready(m, 1))
    wait(10)
    hthrow(m.b, 0)
    wait(ready(m, 1))
    wait(25)
    shoot_lasers(m, 0)

@cold
def p1h(m):
    wait(ready(m, 1))
    throw_cards(m, 0)
    wait(ready(m, 1))
    shoot_lasers(m, 0)
    wait(10)
    wait(ready(m, 1))
    wait(despawn_coins(m))
    coins(m, 4, 0)

@cold
def rconj(r, arg):
    conjure(r, 40)
    die(r)

@cold
def rcards(r, arg):
    throw_cards(r, 0)
    wait(13)
    die(r)

@cold
def rhat(r, arg):
    wait(10)
    throw_hat(r, 0)
    wait(30)
    die(r)

@cold
def p23(m):
    if G.hard:
        spawn_reflection(20 if rnd(2) else -20, rconj, 0)
    wait(15)
    conjure(m, 10)
    wait(30)
    wait(ready(m, 1))
    throw_cards(m, 0)
    if G.hard:
        r = spawn_mrefl()
        move(r, r.x + FX(20), r.y, 15, EI)
        fork(r, rcards, 0, 0)
        sfx(30, 2)
    wait(ready(m, 1))
    if G.hard:
        wait(70)
    shoot_lasers(m, not G.hard)
    wait(ready(m, 1))
    wait(despawn_coins(m))
    if G.hard:
        spawn_reflection(20 if G.player.x < FX(40) else -20, rhat, 0)
    coins(m, 4, 0)
    wait(ready(m, 1))

@cold
def b4a(b, arg):                             # (forked with a wait: the cart's first step)
    conjure(b, 0)
    wait(ready(b, 1))

@cold
def b4b(b, arg):
    throw_cards(b, 0)
    wait(20)
    wait(ready(b, 1))

@cold
def b4c(b, arg):
    shoot_lasers(b, 0)
    wait(ready(b, 1))

@cold
def b4d(b, arg):
    wait(despawn_coins(b))
    wait(17)
    coins(b, 3, 1)
    wait(ready(b, 1))

@cold
def p4(m):
    b = G.brefl
    fork(b, b4a, 0, 75)
    conjure(m, 0)
    wait(ready(m, 1))
    wait(20)
    conjure(m, 0)
    wait(ready(m, 1))
    fork(b, b4b, 0, 84)
    throw_cards(m, 0)
    wait(ready(m, 1))
    wait(100)
    fork(b, b4c, 0, 30)
    shoot_lasers(m, 0)
    wait(ready(m, 1))
    wait(50)
    fork(b, b4d, 0, 0)
    wait(despawn_coins(m))
    coins(m, 3, 0)
    wait(ready(m, 1))
    wait(100)

@cold
def rhat2(r, n):
    wait(8 * n)
    throw_hat(r, 0)
    wait(32 - 8 * n)
    wait(reform(r))
    die(r)

@cold
def rlaser(r, lng):
    wait(7)
    shoot_laser(r, lng)
    wait(reform(r))
    die(r)

@cold
def rcards2(r, arg):
    r.c = 1
    appear(r)
    wait(25)
    throw_cards(r, 0)
    wait(60)
    wait(reform(r))
    die(r)

@cold
def p4h(m):
    wait(ready(m, 1))
    wait(10)
    disappear(m.a)
    disappear(m.b)
    for n in range(5):                       # hat barrage
        spawn_reflection(40 - 20 * n, rhat2, n)
    disappear(m)
    wait(145)
    appear(m)
    wait(30)
    for i in range(4):                       # laser barrage
        wait(10)
        disappear(m)
        m.c = 5
        c = rnd_int(0, 7)
        for k in range(3):
            c = (c + 2) % 8
            spawn_reflection(10 * c - 35, rlaser, i == 3)
        if i == 3:
            wait(40)
            spawn_reflection(-50, rcards2, 0)
            wait(177)
        else:
            wait(66)
        m.c = 1
        appear(m)

@cold
def decide(m, arg):                          # decide_next_action, forever
    while True:
        p = G.phase
        if p == 1:
            if G.hard:
                p1h(m)
            else:
                p1(m)
        elif p == 2 or p == 3:
            p23(m)
        elif p == 4:
            if G.hard:
                p4h(m)
            else:
                p4(m)
        else:
            wait(1)


# ---- phase_change ---------------------------------------------------------------------
@cold
def tap1(l, arg):
    wait(9)
    l.b = 5
    wait(4)
    l.b = 4

@cold
def tap2(l, arg):
    m = l.a
    x = l.x
    y = l.y
    wait(move4(l, m.x - FX(5), m.y - FX(3), 10, EO, 0, -10, -10, -2))
    wait(2)
    wait(move4(l, x, y, 10, EI, -10, -2, 0, -10))

@cold
def pc0(m):                                  # the boss shows up
    l = m.a
    r = m.b
    wait(66)
    wait(appear(l))
    wait(20)
    for i in range(2):                       # shake a finger
        l.b = 5
        wait(3)
        l.b = 4
        wait(3)
    wait(20)
    wait(appear(r))                          # grab the handle
    wait(10)
    wait(move4(r, r.x - FX(16), r.y + FX(8), 10, EO, 10, 0, 10, 5))
    r.b = 2
    m.c = 5                                  # show the face
    wait(33)
    m.c = 6
    wait(28)
    m.c = 5
    wait(34)
    m.c = 1
    wait(5)
    fork(l, tap1, 0, 0)                      # tap the mirror
    fork(l, tap2, 0, 0)
    wait(10)
    m.fl2 |= M_HAT                           # poof! a top hat
    wait(poof(m, 0, -10, 0))
    wait(35)

@cold
def bouq_hide(l, arg):
    wait(10)
    l.b = 3
    sfx(28, 3)
    l.fl2 &= ~H_BOUQ
    wait(move(l, l.x - FX(22), l.y + FX(6), 20, EI))

@cold
def pc1(m):                                  # pound, then the bouquet
    l = m.a
    r = m.b
    wait(ready(m, 2))
    wait(30)
    set_idle(m, 0)
    wait(10)
    pound(m)
    pound(m)
    pound(m)
    m.c = 1
    sfx(16, 3)
    l.fl2 |= H_BOUQ
    r.b = 3
    wait(move4(r, r.x + FX(20), r.y - FX(10), 15, EI, -20, -10, -5, 0))
    wait(35)
    wait(move(l, l.x + FX(2), l.y - FX(9), 20, EI))
    m.c = 3
    wait(30)
    m.c = 1
    wait(15)
    fork(l, bouq_hide, 0, 0)
    wait(move4(r, r.x, r.y + FX(7), 20, EOI, -35, -20, -25, 0))
    wait(15)

@cold
def rcircle(r, arg):
    wait(41)
    die(r)

@cold
def pc1h(m):                                 # pound, then 5 reflections circle
    wait(ready(m, 2))
    wait(30)
    set_idle(m, 0)
    wait(10)
    pound(m)
    pound(m)
    pound(m)
    for i in range(1, 6):
        r = spawn_mrefl()
        a = i * 256 // 5
        b = (i + 1) * 256 // 5
        move4(r, r.x, r.y, 40, LIN, 40 * isin(a + 64) // 127, -40 * isin(a) // 127,
              40 * isin(b + 64) // 127, -40 * isin(b) // 127)
        fork(r, rcircle, 0, 0)
    sfx(31, 1)
    wait(75)

@cold
def phase_change(m):
    G.refl_color = 1
    p = G.phase
    if p == 0:
        pc0(m)
    elif p == 1:
        if G.hard:
            pc1h(m)
        else:
            pc1(m)
    elif p == 2:
        wait(ready(m, 2))
        cast(m, 0)
        wait(ready(m, 1))
        wait(60)
    elif p == 3 and not G.hard:
        wait(ready(m, 2))
        cast(m, 1)
        ready(G.brefl, 1)
        wait(ready(m, 1))
        wait(60)

@cold
def intro(m, arg):
    phase_change(m)
    spawn_magic_tile(130)
    G.scene = 0
    G.phealth.fl |= F_VIS
    G.phase += 1
    decide(m, 0)

@cold
def next_phase(m, arg):
    cancel_everything(m)
    appear(m)
    reel(m, 10)
    wait(10)
    m.c = 5
    wait(20)
    phase_change(m)
    spawn_magic_tile(0)
    G.phase += 1
    decide(m, 0)

@cold
def victory(m, arg):
    cancel_everything(m)
    appear(m)
    reel(m, 40)
    cancel_everything(m)
    wait(move(m, FX(40), FX(-20), 15, EI))
    wait(20)
    if G.prefl != NONE:
        poof(G.prefl, 0, 0, 0)
        die(G.prefl)
        G.prefl = NONE
    poof(spawn(T_TOPHAT, FX(40), FX(-20)), 0, 0, 0)
    die(m)
    wait(120)
    G.cur_open = 0                           # close the curtains
    G.cur_dc = 100
    wait(90)
    G.paused = 1
    wait(75)
    i = G.score_idx
    G.score += 380 - G.timer if G.timer < 380 else 0
    if i:
        if G.score > G.rec2:
            G.rec2 = G.score
        if G.timer <= G.rec3 or not G.rec3:
            G.rec3 = G.timer
    else:
        if G.score > G.rec0:
            G.rec0 = G.score
        if G.timer <= G.rec1 or not G.rec1:
            G.rec1 = G.timer
    save()
    spawn(T_VICTORY, FX(63), 0)


# ===================================================================================
# magic tiles and the boss bar (called by game.c when a particle reaches the bar)
# ===================================================================================
@cold
def gain_health():
    sfx(10, 3)
    if G.bh_health >= 60:
        return 0
    G.bh_health += 1
    h = G.bh_health
    G.bh_vis = 1
    G.bh_rainbow = 15
    b = G.boss
    if G.phase == 0:
        if h == 25:
            G.boss = spawn_mirror(T_MIRROR)
        elif h == 37:
            b.fl |= F_VIS
        elif h == 60:
            fork(b, intro, 0, 1)             # next frame, from the top (stack depth)
    elif h >= 60:
        if G.phase >= 5:
            G.bh_health = 0
        elif G.phase == 4:
            kill_threads()
            G.phase = 5
            G.brefl = NONE
            for i in range(1, 11):
                spawn_magic_tile(20 + 13 * i)
            fork(b, victory, 0, 1)
        else:
            fork(b, next_phase, 0, 1)


# ===================================================================================
# the player
# ===================================================================================
@cold
def step(d):
    if G.p_step_dir != NONE or G.p_teeter or G.player.dc or G.p_stun:
        return 0
    if not G.bh_health and not G.phase and G.boss == NONE:
        sfx(29, 1)
    G.p_facing = d
    G.p_step_dir = d
    G.p_step = 4
    G.p_next = NONE
    return 1

def check_inputs():
    for d in range(4):
        if btnp(d) and not step(d):
            G.p_next = d

@cold
def frozen():                                # called by game.c during freeze frames
    p = G.player
    if p != NONE and not p.fl & F_DEAD:
        check_inputs()

@cold
def undo_step(e):
    e.x = FX(10) * G.p_col - FX(5)
    e.y = FX(8) * G.p_row - FX(4)
    G.p_step = 0
    G.p_step_dir = NONE
    G.p_next = NONE

def copy_player(r):
    p = G.player
    r.x = FX(80) - p.x
    r.y = p.y
    r.dc = p.dc
    r.inv = p.inv
    r.alive = p.alive

@cold
def bump():
    sfx(20, 2)
    undo_step(G.player)
    G.player.dc = 11
    shake(0, 5)

@cold
def get_bumped(c):                           # a coin
    if c != NONE:
        c.idle -= 1
        if not c.idle:
            die(c)

@cold
def get_hurt():
    p = G.player
    if p.inv:
        return 0
    sfx(17, 0)
    p.layer = 11
    shake(6, 10)
    G.ph_anim = 2
    G.ph_dc = 20
    p.inv = 60
    G.p_stun = 19
    G.mult = 0
    G.hearts -= 1
    if G.hearts:
        return 0
    # game over (the cart's promises = {}, figment, game over screen)
    kill_threads()
    G.paused = 1
    h = G.phealth
    h.layer = 16
    h.c = 65                                 # frames until it moves
    f = spawn(T_FIGMENT, p.x + FX(23), p.y + FX(65))
    G.figment = f
    f.c = 35
    s = spawn(T_GAMEOVER, FX(63), 0)
    s.a = 220
    G.cur_open = 0
    G.cur_dc = 100
    die(p)

def player_update(e):
    if G.p_stun:
        G.p_stun -= 1
    if G.p_teeter:
        G.p_teeter -= 1
    check_inputs()
    if G.p_next != NONE and G.p_step_dir == NONE:
        step(G.p_next)
    G.p_col = col(e.x)
    G.p_row = row(e.y)
    if G.p_stun:
        return 0
    while G.p_step_dir != NONE:              # apply_step
        d = G.p_step_dir
        n = G.p_step
        if d > 1:
            e.y += FX((2 * d - 5) * (n - 1 if n > 2 else n))
        else:
            e.x += FX(n if d else -n)
        G.p_step -= 1
        if G.p_step:
            break
        G.p_step_dir = NONE
        if G.p_next == NONE:
            break
        step(G.p_next)
    c = col(e.x)
    r = row(e.y)
    occ = occupant(e)
    rf = G.prefl
    if G.p_col == c and G.p_row == r:
        return 0
    if c < 1 or c > 8 or r < 1 or r > 5:
        sfx(19, 3)
        undo_step(e)
        G.p_teeter = 11
    if occ != NONE or (rf != NONE and (G.p_col < 5) != (c < 5)):
        if rf != NONE:
            copy_player(rf)
            get_bumped(occupant(rf))
        bump()
        get_bumped(occ)
        if rf != NONE:
            copy_player(rf)

def prefl_update(e):
    c = col(e.x)
    r = row(e.y)
    copy_player(e)
    o = occupant(e)
    if (c != col(e.x) or r != row(e.y)) and o != NONE:
        get_bumped(o)
        get_bumped(occupant(G.player))
        bump()
        copy_player(e)


# ===================================================================================
# the screens (the cart's entities 5-9)
# ===================================================================================
@cold
def slide(e, d):
    move4(e, e.x - FX(129) * d, e.y, 100, LIN, 70 * d, 0, 0, 0)

@cold
def start_game(p):                           # the curtains open after 35 frames
    sfx(9, 3)
    G.start = 35
    G.start_phase = p

@cold
def show_title(d):
    t = G.title
    t.x = FX(-66) if d < 0 else FX(192)
    slide(t, d)
    G.starting = 1
    t.a = 115
    G.score_idx = 0

@cold
def pressed(d):                              # btnp(), with A / B as right / left
    return btnp(d) or btn_new() & (BTN_A if d else BTN_B)

@cold
def screen_update(e):
    t = e.type
    f = e.alive
    if t == T_TITLE and e.b:
        G.hard = 0
    if t == T_VICTORY:
        if f == 135 or f == 170:
            sfx(24, 3)
        i = G.score_idx
        if f == 215 and (G.score >= (G.rec2 if i else G.rec0) or G.timer <= (G.rec3 if i else G.rec1)):
            sfx(9, 3)
    if e.a:                                  # check_for_activation
        e.a -= 1
        if not e.a:
            e.b = 1
    if not e.b:
        return 0
    right = pressed(1)
    left = not right and pressed(0)
    if t == T_TITLE:
        if left and G.rec0:
            G.hard = 1
            G.score_idx = 2
        elif not right:
            return 0
        else:
            sfx(24, 3)
        e.b = 0
        slide(e, 1 if right else -1)
        G.score = 40 if G.starting else 0
        G.timer = 0
        keep_only(NONE, NONE, NONE)
        start_game(G.starting)
    elif t == T_GAMEOVER:
        if not right and not left:
            return 0
        d = 1 if right else -1
        if right:
            sfx(24, 3)
        e.b = 0
        slide(e, d)
        h = G.phealth
        f = G.figment
        if h != NONE:
            slide(h, d)
        if f != NONE:
            slide(f, d)
        if left:
            show_title(-1)
            return 0
        G.score = 40 if G.phase <= 1 else 0
        if G.phase <= 1:
            G.timer = 0
        keep_only(e, h, f)
        e.ftd = 100
        h.ftd = 100
        f.ftd = 100
        start_game(G.phase)
    elif right:                              # victory -> credit -> title
        sfx(24, 3)
        e.b = 0
        slide(e, 1)
        if t == T_CREDIT:
            show_title(1)
        else:
            c = spawn(T_CREDIT, FX(192), 0)
            c.a = 130
            slide(c, 1)


# ===================================================================================
# entity classes: update, hurt, death (called by game.c)
# ===================================================================================
def update(e, dc_zero):                      # 1 when the entity moved itself
    t = e.type
    if t >= T_TITLE and t <= T_GAMEOVER:
        screen_update(e)
    elif t == T_FIGMENT:
        if e.c:
            e.c -= 1
            if not e.c:
                move(e, FX(63), FX(72), 60, LIN)
    elif t == T_PHEALTH:
        if e.c:
            e.c -= 1
            if not e.c:
                move4(e, FX(62), FX(45), 60, LIN, -60, 10, -40, 10)
    elif t == T_PLAYER:
        player_update(e)
        return 1
    elif t == T_PREFL:
        prefl_update(e)
        return 1
    elif t == T_TOPHAT:
        if e.alive % 15 == 0:
            bunny(e)
    elif t == T_SPINHAT:
        e.x += isin(e.alive * 82 // 16 + 64) // 4
        e.y += FX(1)
    elif t == T_BUNNY:
        e.x += e.u
        e.y += e.v
        if e.v < 120:
            e.v += 2
    elif t == T_CARD:
        e.x += e.u
    elif t == T_POINTS:
        if e.alive & 1:
            e.y -= FX(1)
    elif t == T_TILESPAWN:
        if e.ftd == 11:
            sfx(8, 3)
    elif t == T_TILE:
        if occupant(e) != NONE:
            tile_covered(e)
    elif t == T_COIN:
        if e.alive == 26:
            coin_land(e)
        if e.alive == 33:
            e.hit = 1 | 4 << 4
    elif t == T_SLAM:
        if e.alive > 1:
            e.hit = 0
    elif t == T_MIRROR or t == T_MREFL:
        idle_step(e)
    elif t == T_HAND:
        e.layer = 6 if e.fl & F_REFL else (9 if e.fl & F_RIGHT else 8)
        idle_step(e)
    elif t == T_LASER:
        e.x = e.a.x
    return 0

@cold
def bunny(e):
    b = spawn(T_BUNNY, e.x, e.y)
    b.u = (16 + rnd(32)) * (1 if rnd(2) else -1)
    b.v = -16 - rnd(32)
    poof(b, 0, 0, 0)
    sfx(25, 2)

@cold
def tile_covered(e):
    die(e)
    spawn_magic_tile(10)

@cold
def coin_land(e):
    sfx(22, 1)
    e.fl |= F_OCC
    e.hit = 5
    shake(2, 2)
    if G.hard:
        for d in range(4):
            s = spawn(T_SLAM, FX(mid(5, e.b + 10 * (d * 2 - 1 if d < 2 else 0), 75)),
                      FX(mid(4, e.c + 8 * (d * 2 - 5 if d > 1 else 0), 36)))
            s.a = d
    move4(e, e.x - FX(2), e.y, 8, LIN, 0, -4, 0, -4)

def idle_step(e):                            # calc_idle_mult: the wobble in u, v
    if e.fl & F_IDLE:
        if e.idle < 20:
            e.idle += 1
    elif e.idle:
        e.idle -= 1
    f = e.alive
    n = 2
    if e.type == T_HAND:
        f = G.boss.alive + (9 if e.fl & F_RIGHT else 4)
        n = 4
    e.u = e.idle * 3 * -isin(f * 4) // 2540
    if G.bh_rainbow > 12 and n == 2:
        e.u += G.scene % 2 * 2 - 1
    e.v = e.idle * n * -isin(f * 8) // 2540

@cold
def hurt(e):                                 # on_hurt
    t = e.type
    if t == T_PLAYER:
        effect(X_PAIN, e.x, e.y)
        get_hurt()
    elif t == T_PREFL:
        get_hurt()
        copy_player(e)
        effect(X_PAIN, e.x, e.y)
    elif t == T_TILE:                        # collected
        shake(2, 2)
        e.hit = 0
        e.ftd = 6
        if G.mult < 8:
            G.mult += 1
        sfx(9, 3)
        G.score += G.mult
        p = spawn(T_POINTS, e.x, e.y - FX(7))
        p.a = G.mult
        n = 12 if G.phase == 0 else 6
        burst(e, 0, 15 if G.phase >= 5 else 25, 10, n)
        if n + G.bh_health < 60 and G.phase < 5:
            spawn_magic_tile((100 if G.phase < 1 else 120) - mid(0, e.alive, 20))
    elif t == T_HEART:
        sfx(18, 0)
        if G.hearts < 4:
            G.hearts += 1
            G.ph_anim = 1
            G.ph_dc = 10
        burst(e, 0, 6, 4, 0)
        die(e)
    else:
        die(e)

@cold
def on_death(e):
    t = e.type
    if t == T_TILESPAWN:
        shake(0, 1)
        spawn(T_TILE, e.x, e.y)
        burst(e, 0, 4, 4, 0)
    elif t == T_COIN:
        burst(e, 0, 6, 4, 0)
        sfx(21, 1)
    elif t == T_MIRROR or t == T_MREFL:
        die(e.a)
        die(e.b)

@cold
def init():                                  # the title screen
    G.title = spawn(T_TITLE, FX(63), 0)
    G.title.a = 5                            # frames until active
    G.starting = 1 if G.rec0 else 0

@cold
def start_game_now():                        # start_game, after the curtains' 35 frames
    p = G.start_phase
    G.cur_open = 1
    G.cur_dc = 100
    G.mult = 0
    G.phase = p - 1 if p else 0
    G.paused = 0
    pl = spawn(T_PLAYER, FX(45), FX(20))
    G.player = pl
    G.p_facing = 0
    G.p_step_dir = NONE
    G.p_next = NONE
    G.p_step = 0
    G.p_teeter = 0
    G.p_stun = 0
    G.phealth = spawn(T_PHEALTH, FX(12), FX(112))
    G.hearts = 4
    G.ph_anim = 0
    G.bh_health = 0
    G.bh_rainbow = 0
    G.bh_dc = 0
    G.bh_vis = 0
    G.prefl = NONE
    G.figment = NONE
    G.boss = NONE
    G.brefl = NONE
    if not p:
        spawn_magic_tile(180)
        return 0
    m = spawn_mirror(T_MIRROR)
    G.boss = m
    m.fl |= F_VIS
    G.bh_vis = 1
    fork(m, intro, 0, 30)
    if p > 1:
        m.fl2 |= M_HAT
    if p > 3:
        r = spawn(T_PREFL, FX(35), pl.y)
        G.prefl = r
        poof(r, 0, 0, 0)


# ===================================================================================
# drawing (per page: everything here runs 8 times a frame)
# ===================================================================================
def draw_player(e, fl):
    stun = G.p_stun
    teeter = G.p_teeter
    if e.inv % 4 >= 2 and not stun:
        return 0
    facing = G.p_facing
    if e.type == T_PREFL and facing < 2:
        facing = 1 - facing
    dx = 3 + 4 * facing
    dy = 6
    base = SPR_P_LR
    frame = 0
    flip = facing == 0
    if facing == 2:
        base = SPR_P_UP
        dx = 5
    elif facing == 3:
        base = SPR_P_DN
        dx = 5
        dy = 9
    if G.p_step:
        frame = 4 - G.p_step
    if teeter or e.dc:
        frame = 6 if e.dc else 4
        if facing > 1:
            dy += 13 - 5 * facing
        else:
            dx += 4 - facing * 8
        if teeter < 3 and e.dc < 3:
            frame = 5
    i = base + frame
    if stun:
        i = SPR_P_STUN
        dx = 5
        dy = 8
        flip = stun % 6 > 2
    spr_e(e, dx, dy, i, (G_FLIPX if flip else 0) | fl)

def draw(e):
    t = e.type
    f = e.alive
    dz = G_DITHER if e.fl & F_REFL else 0
    if t == T_PLAYER or t == T_PREFL:
        draw_player(e, dz)
    elif t == T_MIRROR or t == T_MREFL:
        if not e.fl & F_RVIS:
            return 0
        vis = e.fl & F_VIS
        if vis:
            spr_w(e, 6, 12, SPR_MIRROR, dz)
        if vis or G.bh_rainbow:
            x = 8 if G.bh_rainbow else e.c
            if x:
                spr_w(e, 5, 7, SPR_FACE + x - 1, (G_FLIPX if x == 5 and f % 4 < 2 else 0) |
                      (G_DITHER if G.bh_rainbow and G.scene & 4 else 0))
        if vis:
            if e.fl2 & M_HAT:
                spr_w(e, 6, 15, SPR_HAT_ON, dz)
            if e.dc & 1:
                y = sy(e.y) + 5
                while y < 64:
                    pix(sx(e.x), y, 1)
                    y += 2
    elif t == T_HAND:
        if not e.fl & F_VIS:
            return 0
        r = e.fl & F_RIGHT
        fl = (G_FLIPX if r else 0) | dz
        if e.fl2 & H_BOUQ:
            spr_w(e, 1, 12, SPR_BOUQUET, 0)
        spr_w(e, 7 if r else 4, 8, SPR_HAND + e.b - 1, fl)
        if e.fl2 & H_WAND:
            if e.b == 1:
                spr_w(e, 10 if r else -4, 8, SPR_WAND, fl)
            else:
                spr_w(e, 3 if r else 2, 16, SPR_WAND + 1, fl)
    elif t == T_CARD:
        s = f // 4 % 4
        if e.u < 0:
            s = (6 - s) % 4
        spr_e(e, 5, 7, SPR_CARD + s, 0)
    elif t == T_COIN:
        s = f // 7 if f < 28 else 4
        x = sx(FX(e.b))
        y = sy(FX(e.c))
        i = -s
        while i <= s:
            pix(x + i, y, 1)
            i += 2
        spr_e(e, 4, 5, SPR_COIN + ((5 if e.idle < 3 else 4) if f >= 26 else (2 if f > 10 else 0) + f // 3 % 2), 0)
    elif t == T_TILE:
        x = sx(e.x)                          # the cart's rainbow tile: it blinks solid
        y = sy(e.y)
        rect(x - 4, y - 2, x + 4, y + 2, 1)
        if G.scene & 4:
            fill(x - 3, y - 1, x + 3, y + 1, 1)
    elif t == T_TILESPAWN:
        if e.ftd <= 10:
            d = e.ftd + 3
            x = sx(e.x)
            y = sy(e.y)
            rect(x - d - 1, y - d * 3 // 4, x + d + 1, y + d * 3 // 4, 1)
    elif t == T_LASER:
        x = sx(e.x)
        y = sy(e.y + FX(4))
        soft = e.a.fl & F_REFL
        fill(x - 5, y, x - 5, 63, 0)
        fill(x + 5, y, x + 5, 63, 0)
        for i in range(-4, 5):
            if i == -4 or i == 4 or soft:
                j = y + ((x + i) & 1)
                while j < 64:
                    pix(x + i, j, 1)
                    j += 2
            else:
                fill(x + i, y, x + i, 63, 1)
    elif t == T_SLAM:
        spr_e(e, 5, 3, SPR_SLAM + (e.a < 2) + (e.a == 2) * 2, G_FLIPX if e.a == 0 else 0)
    elif t == T_SPINHAT:
        spr_e(e, 7, 10, SPR_TOPHAT, G_DITHER if e.a.fl & F_REFL else 0)
    elif t == T_TOPHAT:
        spr_e(e, 7, 1, SPR_TOPHAT, 0)
    elif t == T_BUNNY:
        spr_e(e, 7, 7, SPR_BUNNY, G_FLIPX if e.u > 0 else 0)
    elif t == T_HEART:
        if e.ftd > 30 or e.ftd % 4 > 1:
            spr_e(e, 4, 5, SPR_HEART, 0)
    elif t == T_POINTS:
        y = sy(e.y)
        text("00", num(e.a, sx(e.x) - 5, y), y, 1)
    else:
        draw_screen(e, f)

def draw_screen(e, f):                       # on the screen, not the stage
    t = e.type
    x = (e.x >> 4) + G.shake_x
    if t == T_FIGMENT:
        spr(SPR_FIGMENT, x - 5, (e.y >> 5) - 4, G_DITHER if f >= 190 else 0)
    elif t == T_PHEALTH:
        if not e.fl & F_VIS:
            return 0
        y = e.y >> 5
        for i in range(4):
            s = 0                            # SPR_HEART0, SPR_HEART, SPR_HEARTL
            if G.ph_anim == 2 and i == G.hearts:
                if G.ph_dc >= 15 or (G.ph_dc + 1) % 4 < 2:
                    s = 2
            elif i < G.hearts:
                s = 1
            spr(SPR_HEART0 + s, x - 10 + 10 * (i & 1), y - 6 + 6 * (i >> 1), 0)
    elif t == T_TITLE:
        if x < -40 or x > 168:
            return 0
        ctext("just\none\nboss", x, 3, 2)
        prompt(e, "start", 5, 47)
        if f % 30 < 22 and e.b and G.rec0:
            ctext("or < for hard mode", x, 55, 1)
    elif t == T_GAMEOVER:
        prompt(e, "retry", 5, 47)
        if f % 30 < 22 and e.b:
            ctext("or < to return to menu", x, 55, 1)
    elif t == T_CREDIT:
        ctext("thank you for playing!\n\ncreated (with love)\nby bridgs\n\naylanonsen.se", x, 6, 1)
        prompt(e, "continue", 8, 54)
    elif t == T_VICTORY:
        ctext("congrats", x, 1, 2)
        i = G.score_idx
        if f >= 25:
            ctext("you really did it!!" if G.hard else "you did it!", x, 15, 1)
        if f >= 135:
            score_line(x, 27, "score:", G.score, G.timer)
        if f >= 170:
            score_line(x, 35, "best:", G.rec2 if i else G.rec0, G.rec3 if i else G.rec1)
        prompt(e, "continue", 8, 54)
        if e.b and f % 30 < 22:
            if (G.rec2 if i else G.rec0) == G.score:
                text("!", x + 10, 35, 1)
            if (G.rec3 if i else G.rec1) == G.timer:
                text("!", x + 46, 35, 1)

def tick():                                 # the frame's counters (_update)
    if G.scene % 30 == 0 and not G.paused and G.phase > 0 and G.timer < 5999:
        G.timer += 1
    G.scene = G.scene - 12000 if G.scene > 32000 else G.scene + 1
    if G.shake:
        G.shake -= 1
    if G.cur_dc:
        G.cur_dc -= 1
    if G.ph_dc:
        G.ph_dc -= 1
        if not G.ph_dc:
            G.ph_anim = 0
    if G.bh_rainbow:
        G.bh_rainbow -= 1
    if G.bh_dc:
        G.bh_dc -= 1
        if G.bh_health:
            G.bh_health -= 1

def time(s, x, y):                           # m:ss
    x = num(s // 60, x, y)
    x = text(":0" if s % 60 < 10 else ":", x, y, 1)
    return num(s % 60, x, y)

def rect(x0, y0, x1, y1, c):
    fill(x0, y0, x1, y0, c)
    fill(x0, y1, x1, y1, c)
    fill(x0, y0, x0, y1, c)
    fill(x1, y0, x1, y1, c)

def hud():                                   # boss bar, multiplier, score, timer
    if G.bh_vis:
        rect(33, 0, 93, 3, not (G.bh_rainbow and G.scene & 4))
        if G.bh_health:
            fill(34, 1, 33 + (G.bh_health if G.bh_health < 59 else 59), 2, 1)
    if G.mult:
        spr(SPR_MULT, 1, 1, 0)
        num(G.mult, 14, 1)
    s = G.score
    if s:
        w = 4
        while s > 9:
            s = s // 10
            w += 4
        text("00", num(G.score, 120 - w, 1), 1, 1)
    if G.timer:
        time(G.timer, 112 if G.timer < 600 else 108, 59)

def curtains():                              # 0 (open) .. 62 (closed), ease_out_in
    a = 62 * ease(EOI, G.cur_dc * 64 // 25) >> 8
    if not G.cur_open:
        a = 62 - a
    if a:
        fill(0, 0, a + 1, 63, 0)
        fill(126 - a, 0, 127, 63, 0)
        if a < 62:                           # the edges
            fill(a + 1, 0, a + 1, 63, 1)
            fill(126 - a, 0, 126 - a, 63, 1)

def score_line(x, y, label, sc, t):
    text(label, x - 42, y, 1)
    text("00", num(sc, x - 8, y), y, 1)
    time(t, x + 24, y)
