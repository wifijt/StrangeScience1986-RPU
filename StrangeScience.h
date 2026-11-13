// Strange Science (Bally 1986, 6803 MPU): switch, lamp and solenoid numbers for RPU (architecture 5).
// RPU numbers: a switch is the manual's number minus one (op p.1-10), a solenoid its self-test number
// minus one, a lamp PinMAME's number minus one (docs/io-tables.md). All checked on the machine, 2026-11-09.

// Switches
#define SW_RIGHT_OUTLANE        0
#define SW_LEFT_OUTLANE         1
#define SW_EXIT_ANTI_GRAVITY    2
#define SW_ENTER_ANTI_GRAVITY   3
#define SW_BUTTON_LEFT          4    // cabinet button under the left flipper button: Power Saver
#define SW_CREDIT               5
#define SW_BUTTON_RIGHT         6    // cabinet button under the right flipper button: LAB shift
#define SW_OUTHOLE              7
#define SW_COIN_RIGHT           8
#define SW_COIN_LEFT            9
#define SW_COIN_MIDDLE          10
#define SW_LEFT_BUBBLE_LANE     11
#define SW_RIGHT_BUBBLE_LANE    12   // in the shooter lane
#define SW_SLAM                 13
#define SW_TILT                 14
#define SW_SCIENCE_TOP          16
#define SW_SCIENCE_MIDDLE       17
#define SW_SCIENCE_BOTTOM       18
#define SW_STRANGE_TOP          19
#define SW_STRANGE_MIDDLE       20
#define SW_STRANGE_BOTTOM       21
#define SW_RETURN_LANES         24
#define SW_THUMPER_MIDDLE       25
#define SW_THUMPER_LEFT         26
#define SW_THUMPER_RIGHT        27
#define SW_SLING_LEFT           28
#define SW_SLING_RIGHT          29
#define SW_SAUCER               31
#define SW_TROUGH_5             32   // OUT BALL LEFT-5
#define SW_TROUGH_1             36   // OUT BALL RIGHT-1, at the out kicker
#define SW_LOWER_LAB_L          37
#define SW_LOWER_LAB_A          38
#define SW_LOWER_LAB_B          39
#define SW_BRAIN_LEFT           40
#define SW_BRAIN_RIGHT          41
#define SW_UNLOAD_BALL_LANE     42
#define SW_LOAD_BALL_LANE       43
#define SW_REBOUND              44
#define SW_UPPER_LAB_L          45
#define SW_UPPER_LAB_A          46
#define SW_UPPER_LAB_B          47

// Solenoids
#define SOL_THUMPER_MIDDLE      1
#define SOL_THUMPER_LEFT        2
#define SOL_THUMPER_RIGHT       3
#define SOL_SLING_LEFT          4
#define SOL_SLING_RIGHT         5
#define SOL_POWER_SAVER         6    // left ball save kicker post
#define SOL_SAUCER              8
#define SOL_OPEN_ATOM_SMASHER   9    // lowers the ball stop
#define SOL_CLOSE_ATOM_SMASHER  10   // raises it
#define SOL_OUT_KICKER          11   // trough to shooter lane
#define SOL_OUTHOLE             13   // outhole to trough
#define SOL_KNOCKER             14
#define SOL_COIN_METER          12   // "reserved for German use" in the manual; the original pulses it with each coin (bench)
// The continuous outputs (PIA1 PB, low = on), numbered after the coils for the phone's coil test only
#define TEST_NEON               15   // CONTSOL_6803_NEON_OFF
#define TEST_BACKBOX            16   // CONTSOL_6803_BACKBOX_OFF

// Lamps
#define LAMP_STRANGE_S          0
#define LAMP_STRANGE_T          16
#define LAMP_STRANGE_R          32
#define LAMP_STRANGE_A          1
#define LAMP_STRANGE_N          17
#define LAMP_STRANGE_G          33
#define LAMP_STRANGE_E          2
#define LAMP_SCIENCE_S          48
#define LAMP_SCIENCE_C1         64
#define LAMP_SCIENCE_I          80
#define LAMP_SCIENCE_E1         49
#define LAMP_SCIENCE_N          65
#define LAMP_SCIENCE_C2         81
#define LAMP_SCIENCE_E2         50
#define LAMP_VOLTS              13   // flasher
#define LAMP_ATOMIC_GEN         11   // flasher
#define LAMP_THUMPER_LEFT       40
#define LAMP_THUMPER_MIDDLE     24
#define LAMP_THUMPER_RIGHT      9
#define LAMP_LAB_L              89
#define LAMP_LAB_A              58
#define LAMP_LAB_B              74
#define LAMP_COLLECT_BONUS      37
#define LAMP_SAVER_ON           18
#define LAMP_ANTI_GRAVITY       7
#define LAMP_LOAD_ATOMS_LEFT    8    // the atom smasher's lock is open
#define LAMP_LOAD_ATOMS_RIGHT   38
#define LAMP_ATOM_SMASHER_ARROW 6
#define LAMP_BACK_LEFT          75   // backbox flashers
#define LAMP_BACK_MIDDLE        61
#define LAMP_BACK_RIGHT         59

#define LAMP_SPARK_BONUS        22   // the upper-left Spark Bonus (collected by each load)
#define LAMP_SHOOT_AGAIN        23   // SPSA
#define LAMP_HOLD_BONUS         39   // the listing's "HOLE BONUS" is a misprint (the insert and the rules: Hold Bonus)
#define LAMP_PARTICLE_SEP       42
#define LAMP_PARTICLE_RIGHT     44   // flasher
#define LAMP_PARTICLE_LEFT      92   // flasher
#define LAMP_SPARK_25K          43   // flasher
#define LAMP_SPARK_50K          91   // flasher
#define LAMP_SPARK_75K          28   // flasher
#define LAMP_AG_ENTRANCE        76   // flasher
#define LAMP_AG_EXIT            12   // flasher
#define LAMP_ATOM_TOP           27   // flasher
#define LAMP_ATOM_BOTTOM        60   // flasher
#define LAMP_BRAIN_LEFT         71
#define LAMP_BRAIN_RIGHT        87
#define LAMP_MATTER_TRANS       90
#define LAMP_SPECIAL            66
#define LAMP_EXTRA_BALL         94
#define LAMP_BALL_KICKER        46

const byte LampsStrange[7] = {LAMP_STRANGE_S, LAMP_STRANGE_T, LAMP_STRANGE_R, LAMP_STRANGE_A, LAMP_STRANGE_N,
                              LAMP_STRANGE_G, LAMP_STRANGE_E};
const byte LampsScience[7] = {LAMP_SCIENCE_S, LAMP_SCIENCE_C1, LAMP_SCIENCE_I, LAMP_SCIENCE_E1, LAMP_SCIENCE_N,
                              LAMP_SCIENCE_C2, LAMP_SCIENCE_E2};
const byte LampsVolts[9] = {34, 3, 19, 35, 4, 20, 36, 5, 21};                 // 100V to 900V
const byte LampsBonusUnits[9] = {82, 51, 67, 83, 52, 68, 84, 53, 69};         // BONUS 1K to 9K
const byte LampsBonusTens[5] = {85, 54, 70, 86, 55};                          // BONUS 10K to 50K
const byte LampsBonusMult[5] = {56, 72, 88, 57, 73};                          // BONUS MULT 2X to 6X
const byte LampsBubbleLeft[4] = {29, 45, 14, 30};                             // 5K, 10K, 15K, 25K
const byte LampsBubbleRight[4] = {77, 93, 62, 78};
const unsigned long BubbleValues[4] = {5000, 10000, 20000, 50000};   // manual and bench; the inserts say 15K, 25K
const byte LampsPlayMult[4] = {25, 41, 10, 26};                               // PLAY MULT 3X, 6X, 9X, 12X
const byte LampsSpark[3] = {LAMP_SPARK_25K, LAMP_SPARK_50K, LAMP_SPARK_75K};

// Coils fired straight from their switches by RPU's interrupt. All are priority switches (turbo): each fires
// on its first closed sample instead of waiting for a debounced closure (12-18 ms on the 6803, where each
// switch column is read every 6 ms). The Power Saver is a flipper-like kicker on the left cabinet button, in
// the table only while it's lit: the game switches between the two tables (SetSaverArmed).
#define NUM_SWITCHES_WITH_TRIGGERS          5
#define NUM_PRIORITY_SWITCHES_WITH_TRIGGERS 5
struct PlayfieldAndCabinetSwitch SolenoidAssociatedSwitches[] = {
  {SW_SLING_LEFT, SOL_SLING_LEFT, 4},
  {SW_SLING_RIGHT, SOL_SLING_RIGHT, 4},
  {SW_THUMPER_MIDDLE, SOL_THUMPER_MIDDLE, 4},
  {SW_THUMPER_LEFT, SOL_THUMPER_LEFT, 4},
  {SW_THUMPER_RIGHT, SOL_THUMPER_RIGHT, 4},
};
#define NUM_SWITCHES_WITH_SAVER             6
struct PlayfieldAndCabinetSwitch SolenoidAssociatedSwitchesWithSaver[] = {
  {SW_BUTTON_LEFT, SOL_POWER_SAVER, 4},
  {SW_SLING_LEFT, SOL_SLING_LEFT, 4},
  {SW_SLING_RIGHT, SOL_SLING_RIGHT, 4},
  {SW_THUMPER_MIDDLE, SOL_THUMPER_MIDDLE, 4},
  {SW_THUMPER_LEFT, SOL_THUMPER_LEFT, 4},
  {SW_THUMPER_RIGHT, SOL_THUMPER_RIGHT, 4},
};

// Sound board (Turbo Cheap Squeak) commands. Names from the owner's sound tester (2026-11-09); which moment
// plays which from pinnode-hardware's recordings of the original game (2026-11-10)
#define SND_STOP                255
#define SND_DRONE_1             16   // 16 up: the background drone, one step per volts level
#define SND_DRONE_LAST          24
#define SND_COIN_1              60   // 60-63: coin drops; bubble lanes by value, replays
#define SND_COIN                127  // a coin (bench: each coin, with coil 12)
#define SND_STUCK_SWITCH        43   // a switch closed at power-up, once a second with its name (bench)
#define SND_BALLS_NOT_FOUND     44   // eight times with ALL BALLS NOT FOUND (bench; the score count's sound too)
#define SND_PLAYER_ADDED        126  // each player added (bench: rmsg-p2, p3)
#define SND_START_GUITAR_1      83   // 83-87: game start, one per STRANGE SCIENCE step
#define SND_START_VOICE         88   // Strange Science voice, after the guitars
#define SND_ANTI_GRAVITY        72   // x6 before each serve
#define SND_SERVE_0             93   // ball serve, none locked
#define SND_SERVE_1             94   // 1-2 locked
#define SND_SERVE_3             95   // 3 or more locked
#define SND_GUITAR_RIFF         110  // often as a ball goes into play
#define SND_THUMPER             48
#define SND_SLING_LEFT          50
#define SND_SLING_RIGHT         56
#define SND_RETURN_LANE         57
#define SND_RIGHT_OUTLANE       58
#define SND_LEFT_OUTLANE        58   // both outlanes (bench)
#define SND_AG_ENTER            76
#define SND_AG_EXIT             77   // doo whip
#define SND_LADDER_COLLECT      116  // the Anti-Gravity ladder: Collect Bonus lit (and 6th on)
#define SND_LADDER_HOLD         114  // Hold Bonus lit
#define SND_LADDER_EB_LIT       117  // Extra Ball lit
#define SND_EXTRA_BALL          123  // Extra Ball awarded
#define SND_LADDER_SPECIAL      118  // Special lit
#define SND_COUNT               44   // a collect or the bonus counting into the score; a score moving left
#define SND_SCORE_BACK          91   // a drained score moving back to its display after the count
#define SND_BONUS_TO_SCORE      121  // before the bonus counts into the score
#define SND_LETTER              106  // a STRANGE / SCIENCE letter
#define SND_LETTER_LIT          51   // a target on a letter already lit
#define SND_WORD_STRANGE        108  // STRANGE complete (bench)
#define SND_WORD                109  // SCIENCE complete (bench)
#define SND_WORD_DONE_HIT       70   // a target on a completed word
#define SND_COLLECT_LIT         74   // the saucer collecting the bonus
#define SND_STRANGE_SCIENCE     111
#define SND_LAB_LETTER          55   // a new L-A-B letter (bench)
#define SND_LAB_LIT             53   // one already lit (to confirm)
#define SND_LAB_DONE            110  // L-A-B complete (bench)
#define SND_BRAIN_LEFT          102
#define SND_BRAIN_RIGHT         103
#define SND_SAUCER              75
#define SND_COLLECT             73   // bubbles bonus
#define SND_BALL_LOCKED         107
#define SND_MULTIBALL           30
#define SND_SMASH               25   // with 152
#define SND_TIMED_EB_OVER       29   // the smash's timed Extra Ball over (timed out, or 4 s after it's collected)
#define SND_EFFECT              152  // with every Anti-Gravity exit, separator hole, load and unload
#define SND_MB_RELEASE_1        69   // the five-ball release starting
#define SND_MB_RELEASE_2        26
#define SND_ATOM_OUT            120  // one per atom released
#define SND_MB_LOOP_1           72   // through multiball: 8 of these 0.2 s apart about every 8.3 s
#define SND_MB_LOOP_2           80   // and this every 2 s
#define SND_POWER_SAVER         79   // with every kick
#define SND_DRAIN               113  // evil laugh down, 0.6 s after the outhole kick
#define SND_BONUS_COUNT         89   // engine
#define SND_BONUS_DONE          90   // bow whip
#define SND_TILT_WARNING        41   // 35 is tilt, 39 slam tilt
#define SND_TILT                35
#define SND_SLAM                39
#define SND_REPLAY              124  // with the knocker
#define SND_MATCH_SPIN          96   // low saw, x15 like a wheel
#define SND_GAME_OVER           100  // after the match, matched or not (124 and a knock for each match)
#define SND_ENTER_INITIALS      97   // a high score's initials entry starts
