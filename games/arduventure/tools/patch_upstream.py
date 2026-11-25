"""Copies TEAM a.r.g.'s sketch (upstream/ARDU_AB) to build/src and applies the port's
changes to their code (game code: MIT). Each change must match exactly once, so an
upstream change that breaks a patch stops the build instead of silently misbuilding.

    python tools/patch_upstream.py upstream/ARDU_AB build/src
"""
import os, re, shutil, sys

PATCHES = [
  # stateMenuContinue: the game stops the title music and lets the region pick a song,
  # but interiors keep what plays (the song carried in from outside): after continuing
  # in an interior there was silence until walking out. Start the song an interior
  # normally carries: the garden's in your house, the forest's in the tree, the
  # fields' in the village interiors.
  ('menu.h',
   '''  loadGame();
  gameState = STATE_GAME_PLAYING;
  ATM.stop();''',
   '''  loadGame();
  gameState = STATE_GAME_PLAYING;
  ATM.stop();
  songPlaying = 0;
  switch (player.currentRegion)
  {
    case REGION_YOUR_INTERIOR:
      songPlaying = 6;
      ATM.play(nameSong);
      break;
    case REGION_TREE_INTERIOR:
      songPlaying = 3;
      ATM.play(darkForest);
      break;
    case REGION_HOUSE_INTERIOR:
    case REGION_INN_INTERIOR:
    case REGION_SHOP_INTERIOR:
      songPlaying = 1;
      ATM.play(fieldSong);
      break;
  }'''),
  # drawMap: the minimap's tiles row by row instead of column by column (they do not
  # overlap: the same pixels), so each row is one display-list run, not 17 entries
  ('game.h',
   '''      for (byte miniMapOnScreenX = 128; miniMapOnScreenX <= 128; miniMapOnScreenX -= 8)
      {
        //for (byte miniMapOnScreenY = 0; miniMapOnScreenY <= 64; miniMapOnScreenY += 8)
        for (byte miniMapOnScreenY = 64; miniMapOnScreenY <= 64; miniMapOnScreenY -= 8)''',
   '''      for (byte miniMapOnScreenY = 64; miniMapOnScreenY <= 64; miniMapOnScreenY -= 8)
      {
        for (byte miniMapOnScreenX = 128; miniMapOnScreenX <= 128; miniMapOnScreenX -= 8)'''),
  # checkBattle: the encounter's setup runs in the battle group (BATTLE_SETUP below)
  ('battles.h',
   """      isBoss = false;
      setupBattle();""",
   """      isBoss = false;
      gameState = STATE_GAME_BATTLE;
      battlePending = true;"""),
  ('globals.h',
   """bool isBoss = false;""",
   """bool isBoss = false;
bool battlePending = false;                  // port: setupBattle() due (checkInputs_4)"""),
  # findBegin: the start of entry n from an index (tools/textindex.py appends it after
  # the far data) instead of walking n entries of the dictionary, which in the EEPROM
  # costs a read per entry
  ('text.h',
   '''int findBegin(byte searchObject, boolean wordOrSentence)
{''',
   '''extern "C" const unsigned char __far_end[];
int findBegin(byte searchObject, boolean wordOrSentence)
{
  const unsigned char *index = __far_end + 2;
  if (wordOrSentence) index += 2 * pgm_read_word(__far_end);
  return pgm_read_word(index + 2 * searchObject);
}

int findBeginOriginal(byte searchObject, boolean wordOrSentence)
{'''),
  # drawTiles: same picture, but the 9 x 5 visible tile IDs are cached and decoded from
  # the map (far data, in the EEPROM) only when the camera moves to another tile or a
  # map flag changes (opened chests, used amulets) - not 45 map decodes every frame.
  # The tiles go into the display list as one entry (port/platform.cpp, dl_tiles).
  ('worlddata.h',
   '''void drawTiles()
{''',
   '''static byte tileCache[5][9];                   // [row][column] of the visible tiles
static int tileCacheX = 0x7FFF, tileCacheY;
static byte tileCacheFlags[4];                // the map state getTileID reads

// decodes the 9 x 5 visible tiles from the map (far data); in the overworld group, with a
// copy in the title group (tileRefill__1): the groups where the camera moves
void tileRefill(int tx, int ty)
{
  for (byte j = 0; j < 5; j++)
    for (byte i = 0; i < 9; i++)
      tileCache[j][i] = getTileID((uint16_t)((tx + i) << 4), (uint16_t)((ty + j) << 4));
}

void tileRefill__1(int tx, int ty);
typedef void (*TileRefill)(int, int);
const TileRefill tileRefillGroup[5] = { 0, tileRefill__1, tileRefill, 0, 0 };

void drawTiles()
{
  int tx = camX >> 4, ty = camY >> 4;
  if (tx != tileCacheX || ty != tileCacheY ||
      tileCacheFlags[0] != player.gameTriggers[2] || tileCacheFlags[1] != player.gameTriggers[3] ||
      tileCacheFlags[2] != player.lastDoor || tileCacheFlags[3] != (byte)player.bossActiveAlive)
  {
    TileRefill refill = tileRefillGroup[ov_loaded()];         // ov_loaded(): 0..4
    if (refill)                                 // the inventory and battle groups never
    {                                           // move the camera: nothing to decode there
      refill(tx, ty);
      tileCacheX = tx; tileCacheY = ty;
      tileCacheFlags[0] = player.gameTriggers[2]; tileCacheFlags[1] = player.gameTriggers[3];
      tileCacheFlags[2] = player.lastDoor; tileCacheFlags[3] = (byte)player.bossActiveAlive;
    }
  }
#ifdef ARDU_TILECHECK
  for (byte j = 0; j < 5; j++) for (byte i = 0; i < 9; i++)
    if (tileCache[j][i] != getTileID((uint16_t)(camX + 16 * i), (uint16_t)(camY + 16 * j))) dbg_tile(i, j);
#endif
  dl_tiles((tileCacheX << 4) - camX, (tileCacheY << 4) - camY, tileBlocks, tileIndex, &tileCache[0][0]);
}

void drawTilesOriginal()
{'''),
  # getRegion: remembers its last answer (a scan of the 80-byte region table, far data);
  # the overworld asks for the player's chunk every frame (checkRegion), getChunk for the
  # chunks of the tiles it decodes. The region table and the door table below are more
  # than the far cache holds: scanning them every frame missed on every line (~2300
  # EEPROM reads per second in the overworld, about half the CPU time).
  ('worlddata.h',
   '''byte getRegion(byte chunk_pos_x, byte chunk_pos_y)
{
  for (byte i = 0; i < TOTAL_REGIONS; i++)
  {
    Point chunk_pos = { .x = chunk_pos_x, .y = chunk_pos_y};
    Rect reg = {.x = pgm_read_byte(regions + i * 4), .y = pgm_read_byte(regions + 1 + i * 4),
                .width = pgm_read_byte(regions + 2 + i * 4), .height = pgm_read_byte(regions + 3 + i * 4)
               };
    if (arduboy.collide(chunk_pos, reg)) return i;
  }
  return REGION_FOREST;
}''',
   '''#ifndef REGION_MEMO
#define REGION_MEMO 2
#endif
static uint32_t regionMemo[REGION_MEMO];      // last answers, newest first: (chunk | 0x10000) << 8 | region
byte getRegion(byte chunk_pos_x, byte chunk_pos_y)
{
  uint32_t key = 0x10000 | chunk_pos_x << 8 | chunk_pos_y;
  for (byte k = 0; k < REGION_MEMO; k++)
    if ((regionMemo[k] >> 8) == key) return (byte)regionMemo[k];
  byte r = REGION_FOREST;
  for (byte i = 0; i < TOTAL_REGIONS; i++)
  {
    Point chunk_pos = { .x = chunk_pos_x, .y = chunk_pos_y};
    Rect reg = {.x = pgm_read_byte(regions + i * 4), .y = pgm_read_byte(regions + 1 + i * 4),
                .width = pgm_read_byte(regions + 2 + i * 4), .height = pgm_read_byte(regions + 3 + i * 4)
               };
    if (arduboy.collide(chunk_pos, reg)) { r = i; break; }
  }
  for (byte k = REGION_MEMO - 1; k; k--) regionMemo[k] = regionMemo[k - 1];
  regionMemo[0] = key << 8 | r;
  return r;
}'''),
  # checkDoors: the player is in door i's rectangle (16 * x - 3, 16 * y, 10 x 10) exactly
  # when (player.x + 3, player.y) is in tile (x, y) at offset < 10 in both directions, so
  # the door table (102 bytes, far data) is searched once per tile the player enters, for
  # the first door with that tile: the same door as the original scan.
  ('worlddata.h',
   '''    Point p =
    {
      .x = player.x,
      .y = player.y,
    };

    for (byte i = 0; i < TOTAL_DOORS; i++)
    {
      Rect door =
      {
        .x = (16 * pgm_read_byte(&doors[i * 2])) - 3,
        .y = 16 * pgm_read_byte(&doors[(i * 2) + 1 ]),
        .width = 10,
        .height = 10
      };

      if (arduboy.collide(p, door))
      {
        if (playerDirection == FACING_NORTH) player.lastDoor = i;''',
   '''    static int doorTile;                      // tile searched last (key, 0: none)
    static byte doorAt;                       // its door + 1 (0: no door there)
    int tx = (int16_t)(player.x + 3) >> 4, ty = (int16_t)player.y >> 4;
    int key = (tx << 16) + ty + 1;            // coordinates >= 0: key != 0
    if (key != doorTile)
    {
      doorTile = key; doorAt = 0;
      for (byte i = 0; i < TOTAL_DOORS; i++)
        if (pgm_read_byte(&doors[i * 2]) == tx && pgm_read_byte(&doors[(i * 2) + 1 ]) == ty) { doorAt = i + 1; break; }
    }
    if (doorAt)
    {
      byte i = doorAt - 1;
      if (((player.x + 3) & 15) < 10 && (player.y & 15) < 10)
      {
        if (playerDirection == FACING_NORTH) player.lastDoor = i;'''),
]

def chunk_table(dst):
    """getChunk's switch over 54 map positions (houses, trees, shops...: chunk
    x + 32 * y -> chunk 128..191) as a table in flash, 10-bit position + 6-bit chunk - 128
    per entry: the switch compiled to a compare chain of ~430 bytes in each of getChunk's
    three copies (title, overworld, menu groups)."""
    p = os.path.join(dst, 'worlddata.h'); s = open(p).read()
    head = '  switch (chunk_pos_x + chunk_pos_y * 32)\n  {'
    if s.count(head) != 1: sys.exit('getChunk: switch not found')
    a = s.index(head); e = s.index('\n  }\n', a) + 5
    entries, pending = [], []
    for line in s[a + len(head):e].split('\n'):
        line = line.split('//')[0].strip()
        if m := re.match(r'^case (\d+):$', line): pending.append(int(m.group(1)))
        elif m := re.match(r'^return (\d+);$', line):
            r = int(m.group(1)); assert 128 <= r < 192
            entries += [pos | (r - 128) << 10 for pos in pending]; pending = []
        elif line not in ('', 'break;', '}'): sys.exit(f'getChunk: unexpected line {line!r}')
    assert not pending and all(v & 0x3FF < 1024 for v in entries)
    table = ('// port: getChunk\'s specific chunks (chunk_table): position x + 32 * y | (chunk - 128) << 10\n'
             'const uint16_t NEAR chunkSpecial[] = { %s };\n\n') % ', '.join(str(v) for v in entries)
    lookup = ('  for (byte i = 0; i < sizeof chunkSpecial / 2; i++)\n'
              '    if ((chunkSpecial[i] & 0x3FF) == chunk_pos_x + chunk_pos_y * 32) return 128 + (chunkSpecial[i] >> 10);\n')
    f = s.rindex('\nbyte getChunk(', 0, a) + 1
    s = s[:f] + table + s[f:a] + lookup + s[e:]
    open(p, 'w').write(s)


# data read every frame stays in flash (NEAR); all other PROGMEM data goes to the EEPROM
NEAR = ['font']                                # (tiles: dedupe_tiles, always NEAR)

def apply_patches(src, dst):
    os.makedirs(dst, exist_ok=True)
    for f in os.listdir(src):
        shutil.copy(os.path.join(src, f), os.path.join(dst, f))
    for f, old, new in PATCHES:
        p = os.path.join(dst, f); s = open(p).read()
        n = s.count(old)
        if n != 1: sys.exit(f'patch for {f} matches {n} times (upstream changed?)')
        open(p, 'w').write(s.replace(old, new))
    import re
    for f in os.listdir(dst):
        p = os.path.join(dst, f); s = open(p).read(); o = s
        for name in NEAR:
            # the declaration line of `name[]`: its PROGMEM becomes NEAR (flash)
            s = re.sub(r'^([^\n]*\b%s\[\][^\n]*)$' % name,
                       lambda m: m.group(1).replace('PROGMEM', 'NEAR'), s, flags=re.M)
        if s != o: open(p, 'w').write(s)
    print(f'{len(PATCHES)} patches applied to {dst}, kept in flash: {", ".join(NEAR)}')


# ---- overlay groups -------------------------------------------------------------------
# Game states by code group: every state's code is loaded into the flash overlay window
# with its group (see port/overlay.cpp). Group 0 is the resident code.
GROUPS = {
  1: ['STATE_MENU_INTRO', 'STATE_MENU_MAIN', 'STATE_MENU_CONTINUE', 'STATE_MENU_NEW',
      'STATE_MENU_SOUND', 'STATE_MENU_CREDITS', 'STATE_GAME_INTRO', 'STATE_GAME_NEW',
      'STATE_GAME_OVER', 'STATE_REBOOT'],                                   # title, new game
  2: ['STATE_GAME_PLAYING', 'WALKING_THROUGH_DOOR'],                       # overworld
  3: ['STATE_GAME_INVENTORY', 'STATE_GAME_EQUIP', 'STATE_GAME_STATS', 'STATE_GAME_MAP',
      'STATE_GAME_OBJECTS',                                                 # (chests, signs: rare)
      'STATE_GAME_ITEMS', 'STATE_GAME_WEAPON', 'STATE_GAME_ARMOR', 'STATE_GAME_AMULET',
      'STATE_GAME_SAVE', 'STATE_GAME_SOUND', 'STATE_GAME_END', 'STATE_GAME_SHOP',
      'STATE_GAME_INN'],                                                    # menus, shop, inn
  4: ['STATE_GAME_BATTLE', 'STATE_GAME_BOSS', 'STATE_BATTLE_REWARDS'],      # battles
}


# A random encounter starts in the overworld (checkBattle, group 2), but the battle's
# setup belongs to the battle group: checkBattle only switches the state and flags it,
# and the battle group's input step - later in the same frame - runs setupBattle().
BATTLE_SETUP = '  if (battlePending)\n  {\n    battlePending = false;\n    setupBattle();\n  }\n'


# functions reached only through tables the call graph does not follow
EXTRA_ROOTS = {1: ['tileRefill__1'], 2: ['tileRefill']}

# per-group copies (name__g) of functions several groups need, so they can leave the
# resident code: the title group decodes the map too (game intro)
CLONES = {1: ['getChunkBit', 'getRegion', 'getChunk', 'getTileID', 'tileRefill', 'toggleSound'],
          3: ['getChunkBit', 'getRegion', 'getChunk', 'toggleSound']}   # map screen, sound
# functions of a group whose calls go to that group's copies
CLONE_CALLERS = {1: ['checkInputs_1'], 3: ['drawMap', 'checkInputs_3']}


def find_function(s, name):
    """(start, end) of the definition of `name` in s, or None"""
    m = re.search(r'^[A-Za-z_][\w ]*?[\s*&]%s\s*\([^;{}]*\)\s*\{' % re.escape(name), s, re.M)
    if not m: return None
    i, depth = m.end(), 1
    while depth:
        depth += {'{': 1, '}': -1}.get(s[i], 0); i += 1
    return m.start(), i


def clone_functions(dst):
    for g, names in CLONES.items():
        for f in sorted(os.listdir(dst)):
            p = os.path.join(dst, f); s = open(p).read(); o = s
            for name in names:
                r = find_function(s, name)
                if not r: continue
                body = s[r[0]:r[1]]
                for other in names:                   # calls between the copies stay copies
                    body = re.sub(r'\b%s(\s*\()' % other, r'%s__%d\1' % (other, g), body)
                body = re.sub(r'\s*=\s*[^,()]+(?=[,)])', '', body.split('{', 1)[0]) + '{' + body.split('{', 1)[1]
                s = s[:r[1]] + '\n\n' + body + s[r[1]:]
            for caller in CLONE_CALLERS.get(g, []):
                r = find_function(s, caller)
                if not r: continue
                body = s[r[0]:r[1]]
                for name in names:
                    body = re.sub(r'\b%s(\s*\()' % name, r'%s__%d\1' % (name, g), body)
                s = s[:r[0]] + body + s[r[1]:]
            if s != o: open(p, 'w').write(s)


def dedupe_tiles(dst):
    """tileSheet (16 x 16 tiles, the only image always in flash) as unique 8 x 8 blocks
    (tileBlocks, an 8 x 8 sprite sheet) and 4 block numbers per tile (tileIndex): the
    same pixels in ~140 bytes less flash. tileSheet0 keeps tile 0 as a plain sprite for
    the one direct use of the sheet (player.h)."""
    p = os.path.join(dst, 'bitmaps.h'); s = open(p).read()
    m = re.search(r'^[^\n]*\btileSheet\[\]\s*=\s*\{(.*?)\};', s, re.S | re.M)
    body = re.sub(r'//[^\n]*', '', m.group(1))
    data = [int(t.strip(), 0) for t in body.split(',') if t.strip()]
    assert data[0] == 16 and data[1] == 16
    tiles = (len(data) - 2) // 32
    blocks, index = {}, []
    for t in range(tiles):
        tile = data[2 + 32 * t:2 + 32 * (t + 1)]
        for by in range(2):
            for bx in range(2):
                b = tuple(tile[by * 16 + bx * 8:by * 16 + bx * 8 + 8])
                index.append(blocks.setdefault(b, len(blocks)))
    assert len(blocks) < 256
    flat = [v for b in sorted(blocks, key=blocks.get) for v in b]
    fmt = lambda vs: ', '.join('0x%02X' % v for v in vs)
    new = ('// port: tileSheet as unique 8 x 8 blocks + 4 block numbers per tile (dedupe_tiles)\n'
           'const unsigned char NEAR tileBlocks[] = { 8, 8, %s };\n'
           'const unsigned char NEAR tileIndex[] = { %s };\n'
           'const unsigned char PROGMEM tileSheet0[] = { 16, 16, %s };') % (fmt(flat), fmt(index), fmt(data[2:34]))
    s = s[:m.start()] + new + s[m.end():]
    open(p, 'w').write(s)
    p = os.path.join(dst, 'player.h'); s = open(p).read()
    if s.count('tileSheet, 0)') < 1: sys.exit('player.h: direct tileSheet use not found')
    s = s.replace('tileSheet, 0)', 'tileSheet0, 0)')
    open(p, 'w').write(s)
    p = os.path.join(dst, 'worlddata.h'); s = open(p).read()        # drawTilesOriginal (unused)
    open(p, 'w').write(re.sub(r'\btileSheet\b(?!0)', 'tileSheet0', s))


def split_check_inputs(dst):
    """checkInputs() is one switch over gameState; make one copy per group holding only
    that group's cases (unchanged), so input code goes into its group's overlay."""
    p = os.path.join(dst, 'inputs.h'); s = open(p).read()
    head = 'void checkInputs()\n{'
    a = s.index(head)
    sw = s.index('switch (gameState)', a)
    body_start = s.index('{', sw) + 1
    depth, i = 1, body_start                       # matching brace of the switch
    while depth:
        if s[i] == '{': depth += 1
        elif s[i] == '}': depth -= 1
        i += 1
    body_end = i - 1
    fn_end = s.index('}', i) + 1                   # closing brace of checkInputs
    body = s[body_start:body_end]
    blocks, cur, labels = [], [], []               # (labels, text)
    for line in body.split('\n'):
        m = re.match(r'^    case (\w+):\s*$', line)
        if m and cur and any(l.strip() and not re.match(r'^\s*case \w+:\s*$', l) for l in cur):
            blocks.append((labels, '\n'.join(cur))); cur, labels = [], []
        if m: labels.append(m.group(1))
        cur.append(line)
    blocks.append((labels, '\n'.join(cur)))
    state_group = {st: g for g, sts in GROUPS.items() for st in sts}

    def falls_through(t):                      # no break/return as the block's last statement
        code = [l.split('//')[0].strip() for l in t.split('\n')]
        code = [l for l in code if l and not re.match(r'^case \w+:$', l)]
        return not code or code[-1] not in ('break;', 'return;')
    fall = [s for labels, t in blocks if labels and falls_through(t) for s in labels]
    out = []
    for g in GROUPS:
        # the group's cases in the original order; a case without a final break keeps
        # running into the original next case (whatever its group), as in the original
        mine, prev_falls = [], False
        for labels, t in blocks:
            if labels and (all(state_group.get(l) == g for l in labels) or prev_falls):
                mine.append(t); prev_falls = falls_through(t)
            else:
                prev_falls = False
        pre = BATTLE_SETUP if g == 4 else ''
        out.append('void checkInputs_%d()\n{\n%s  switch (gameState)\n  {%s\n  }\n}\n' % (g, pre, '\n'.join(mine)))
    for labels, t in blocks:
        gs = {state_group.get(l) for l in labels}
        if labels and (len(gs) != 1 or None in gs): sys.exit(f'checkInputs: cases {labels} span groups {gs}')
    table = ('typedef void (*InputFunction)();\n'
             'const InputFunction checkInputsGroup[] = { 0, %s };\n'
             'void checkInputs()\n{\n  ov_ensure(stateGroup(gameState));\n'
             '  checkInputsGroup[stateGroup(gameState)]();\n}\n') % ', '.join('checkInputs_%d' % g for g in GROUPS)
    s = s[:a] + '\n'.join(out) + table + s[fn_end:]
    open(p, 'w').write(s)
    # the state -> group table (resident)
    gtab = ['0'] * 32
    g_src = open(os.path.join(dst, 'globals.h')).read()
    for st, g in state_group.items():
        n = int(re.search(r'#define\s+%s\s+(\d+)' % st, g_src).group(1)); gtab[n] = str(g)
    p = os.path.join(dst, 'globals.h'); s = open(p).read()
    s = s.replace('#define EEPROM_START', 'const byte STATE_GROUP[32] = { %s };\n'
                  'static inline byte stateGroup(byte st) { return STATE_GROUP[st & 31]; }\n'
                  'void ov_ensure(byte group);\nextern "C" uint8_t ov_loaded();\nvoid dbg_state(byte st);\nvoid dbg_tile(byte i, byte j);\n\n#define EEPROM_START' % ', '.join(gtab), 1)
    open(p, 'w').write(s)
    # the main loop: the state's group is loaded before its function runs
    p = os.path.join(dst, 'ARDU_AB.ino'); s = open(p).read()
    old = '  ((FunctionPointer) pgm_read_word(&mainGameLoop[gameState]))();'
    if s.count(old) != 1: sys.exit('loop dispatch not found')
    s = s.replace(old, '  dbg_state(gameState);\n  ov_ensure(stateGroup(gameState));\n' + old)
    open(p, 'w').write(s)


def annotate_overlays(dst, assign_json):
    """Puts every function of an overlay group into section .ovN.text.<name> (the
    assignment comes from tools/overlays.py on a first build)."""
    import json
    assign = json.load(open(assign_json))
    done = set()
    for f in sorted(os.listdir(dst)):
        p = os.path.join(dst, f); s = open(p).read(); o = s
        for name, g in assign.items():
            # a definition: type and name at the start of a line, '{' on that or the next line
            pat = re.compile(r'^((?:[A-Za-z_][\w:<>*& ]*?[\s*&]))(%s)\s*\(([^;{}]*?)\)\s*\n?\s*\{' % re.escape(name), re.M)
            def put(m, g=g, name=name):
                done.add(name)
                return ('__attribute__((section(".ov%d.text.%s"))) ' % (g, name)) + m.group(0)
            s = pat.sub(put, s)
        if s != o: open(p, 'w').write(s)
    return len(done), len(assign)


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    apply_patches(src, dst)
    dedupe_tiles(dst)
    chunk_table(dst)
    split_check_inputs(dst)
    clone_functions(dst)
    msg = 'checkInputs split into groups ' + ', '.join(map(str, GROUPS))
    if len(sys.argv) > 3 and os.path.exists(sys.argv[3]):
        n, total = annotate_overlays(dst, sys.argv[3])
        msg += f'; {n} of {total} overlay functions placed'
    print(msg)
