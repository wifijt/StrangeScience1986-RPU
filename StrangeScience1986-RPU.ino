// Strange Science on RPU: the original game's rules for the PinNode 400-IC in a Bally 6803 MPU
// (RPU_OS_HARDWARE_REV 402, RPU_MPU_ARCHITECTURE 5). docs/rules.md has every rule with its source: Bally's
// feature and scoring pages (pp.1-15 to 1-20, transcribed by pinnode-hardware), the playfield instruction
// card, and pinnode-hardware's recordings of the original running on this machine (2026-11-10). In short:
//  - STRANGE / SCIENCE: a flashing letter sweeps down each word (faster with more volts); a target lights it.
//    Both words light the Power Saver, advance the Spark Bonus (25K, 50K, 75K) and light a timed Special.
//  - Particle Separator (upper L-A-B holes) and Anti-Gravity take turns collecting the Spark Bonus.
//    Anti-Gravity completions light Collect Bonus, then Hold Bonus, Extra Ball and Special.
//  - L-A-B (lanes and holes, right cabinet button shifts them) advances the bonus multiplier 2X to 6X.
//  - Atom smasher: loads collect the Spark Bonus, the first raises the pins. Smash the front ball for
//    multiball and a timed Extra Ball; five loaded, or a load during multiball, releases them all. Balls in
//    play multiply playfield scores 3X, 6X, 9X, 12X. Loaded atoms stay for the next game.
//  - Thumpers advance the volts (13 hits a step, kept from ball to ball).
//  - Power Saver: a flipper-like kicker on the left cabinet button, lit at the start of ball 1, every ball or
//    none (the Power Saver setting), and by STRANGE SCIENCE and a smash. While lit it flips on every press; it goes out 4 s
//    after the ball passes the left outlane.
//  - Replays at 1,500,000 and 3,500,000; beating the high score (or 10,000,000) is 3 replays; match.
// Free play, 3 balls, 1-4 players (credit button). USB serial at 115200 logs switches and states, and takes
// "snd N" (sound board command N), "neon on|off" and "backbox on|off", and the atom smasher tests.
//  - Ball handling: balls in play = the machine's balls less the trough's (and outhole's) and the trapped
//    ones. A release lets the trapped balls out one at a time (pins down for one ball, up, 2 s, down ...).
// Sounds come from the machine's own sound board.

#include "RPU_Config.h"
#include "RPU.h"

// The 14 flashers (phase C/D bulbs on J13): the library owns them (RPU_SetFlasherLamps6803 in setup). Turning
// one on gives a single 50 ms flash, as the original's are; held on, nothing more (holding them on blew the
// flasher fuse and both bulbs on the machine, 2026-11-12)
const byte Flashers[14] = {11, 12, 13, 27, 28, 43, 44, 59, 60, 61, 75, 76, 91, 92};
#include <PinNode.h>          // the PinNode library (build with --library pinnode-rpu)
#include "PinNodeGame.h"      // generated from pinnode.game.json by pinnode-gen
#include "StrangeScience.h"
#include "AttractShow.h"
#include "LampAnimations.h"
#include "OperatorMenus.h"
OperatorMenus Menus;   // the coin-door menus (see RunOperatorMenu)

extern volatile uint32_t RPU_6803_MaxTickUs;   // RPU's longest 1 ms interrupt so far

#ifndef FREE_PLAY                      // the bench can build with -DFREE_PLAY=false to count credits
#define FREE_PLAY             true     // the free-play setting's default
#endif
#define TILT_SETTLE_MS        1000

// The machine states, as the RPU's games have them (ExampleMachine): each runs in its own function (see loop)
#define MACHINE_STATE_ATTRACT         0
#define MACHINE_STATE_INIT_GAMEPLAY   1     // a game starting: on to the first ball
#define MACHINE_STATE_INIT_NEW_BALL   2     // waiting for the outhole to be empty, then the ball's start
#define MACHINE_STATE_NORMAL_GAMEPLAY 4
#define MACHINE_STATE_COUNTDOWN_BONUS 99
#define MACHINE_STATE_BALL_OVER       100   // the next ball, player or the game's end
#define MACHINE_STATE_MATCH_MODE      110   // the game's end: the match, a high score's initials, the scores
#define MACHINE_STATE_DIAGNOSTICS     120   // tests from the phone, or the coin-door self-test button in attract

int MachineState = MACHINE_STATE_ATTRACT;
unsigned long MachineStateTime = 0;
unsigned long Now = 0;

byte NumPlayers = 0, CurrentPlayer = 0, CurrentBall = 0;
unsigned long Scores[4];
unsigned long LastScores[4];
byte LastNumPlayers = 0;
unsigned long HighScore = 0, HighScoreToBeat = 0;
boolean EntryActive = false;   // a high score's initials going in, after the match (see ShowEntry)
byte Credits = 0;

// ------------------------------------------------------------------ settings
// The operator's settings (docs/settings.md), kept in EEPROM from byte 140 and changed three ways: the
// cabinet buttons' menu (the coin-door button, see OperatorMenu), the PinNode app (PinNodeSettingsTable) and,
// to come, the original's ABC keypad screens. The table's order is the PinNode index: never reorder it.
// The Power Saver at the start of a ball (the owner's setting; STRANGE SCIENCE and a smash light it in all three)
#define SAVER_ONCE            0        // each player's ball 1 (Bally's "Power Saver" register, factory 1)
#define SAVER_EARNED          1        // never: only STRANGE SCIENCE and a smash
#define SAVER_EVERY_BALL      2        // every ball, an extra ball too
#define SETTINGS_EEPROM_MARK      0xB4   // 0xB3: before the coin settings
// The settings' EEPROM places (the RPU's way: a name each)
#define EEPROM_SETTINGS_MARK_BYTE          140
#define EEPROM_FREE_PLAY_BYTE              141
#define EEPROM_BALLS_OVERRIDE_BYTE         142
#define EEPROM_CREDIT_LIMIT_BYTE           143
#define EEPROM_REPLAY_1_UL                 144
#define EEPROM_REPLAY_2_UL                 148
#define EEPROM_HIGH_SCORE_REPLAYS_BYTE     152
#define EEPROM_MATCH_FEATURE_BYTE          153
#define EEPROM_TILT_WARNING_BYTE           154
#define EEPROM_SLINGSHOTS_BYTE             155
#define EEPROM_AWARD_COUNT_BYTE            156
#define EEPROM_EXTRA_BALL_TIMER_BYTE       157
#define EEPROM_SPECIAL_TIMER_BYTE          158
#define EEPROM_POWER_SAVER_BYTE            159
struct GameSetting {
  const char *name;             // 14 places: displays 0 and 1, as the original's screens split them
  unsigned short eeprom;
  byte size;                    // 1 or 4 bytes
  long minValue, maxValue, defaultValue, step;
  const char *const *labels;    // the values' names on the display (null: the number)
  boolean difficulty;           // held by a tournament lock
};
const char *const OnOffLabels[] = {"    OFF", "     ON"};
const char *const TimerLabels[] = {"    8 S", "   16 S", "   32 S", "   BALL"};
const char *const SaverLabels[] = {" BALL 1", " EARNED", "  EVERY"};
// Coins per credit, a chute (the RPU's nine: coins / credits)
#define NUM_CPC_PAIRS 9
byte CPCPairs[NUM_CPC_PAIRS][2] = {{1, 5}, {1, 4}, {1, 3}, {1, 2}, {1, 1}, {2, 3}, {2, 1}, {3, 1}, {4, 1}};
const char *const CPCLabels[] = {"  1 / 5", "  1 / 4", "  1 / 3", "  1 / 2", "  1 / 1", "  2 / 3", "  2 / 1", "  3 / 1", "  4 / 1"};
const GameSetting Settings[] = {
  {"   FREE  PLAY ", EEPROM_FREE_PLAY_BYTE, 1, 0, 1, FREE_PLAY ? 1 : 0, 1, OnOffLabels, false},       // 0
  {"BALLS PER GAME", EEPROM_BALLS_OVERRIDE_BYTE, 1, 3, 5, 3, 1, nullptr, true},                          // 1
  {"CREDIT  LIMIT ", EEPROM_CREDIT_LIMIT_BYTE, 1, 1, 40, 20, 1, nullptr, false},                       // 2
  {"FIRST   REPLAY", EEPROM_REPLAY_1_UL, 4, 0, 9999000, 1500000, 1000, nullptr, false},        // 3
  {"SECOND  REPLAY", EEPROM_REPLAY_2_UL, 4, 0, 9999000, 3500000, 1000, nullptr, false},        // 4
  {"HI SCORE AWARD", EEPROM_HIGH_SCORE_REPLAYS_BYTE, 1, 0, 3, 3, 1, nullptr, false},                         // 5: replays for the high score
  {" MATCH  OPTION", EEPROM_MATCH_FEATURE_BYTE, 1, 0, 1, 1, 1, OnOffLabels, false},                     // 6
  {"TILT   WARNING", EEPROM_TILT_WARNING_BYTE, 1, 0, 3, 2, 1, nullptr, true},                          // 7
  {"SLINGSHOT   ON", EEPROM_SLINGSHOTS_BYTE, 1, 0, 1, 1, 1, OnOffLabels, true},                      // 8
  {"AWARD    COUNT", EEPROM_AWARD_COUNT_BYTE, 1, 0, 3, 3, 1, nullptr, true},                          // 9
  {"X BALL   TIMER", EEPROM_EXTRA_BALL_TIMER_BYTE, 1, 0, 2, 2, 1, TimerLabels, true},                      // 10
  {"SPECIAL  TIMER", EEPROM_SPECIAL_TIMER_BYTE, 1, 0, 3, 2, 1, TimerLabels, true},                      // 11
  {"POWER    SAVER", EEPROM_POWER_SAVER_BYTE, 1, 0, 2, SAVER_ONCE, 1, SaverLabels, true},             // 12
  {"COINS  CHUTE 1", RPU_CPC_CHUTE_1_SELECTION_BYTE, 1, 0, 8, 4, 1, CPCLabels, false},   // 13: coins / credits
  {"COINS  CHUTE 2", RPU_CPC_CHUTE_2_SELECTION_BYTE, 1, 0, 8, 4, 1, CPCLabels, false},   // 14
  {"COINS  CHUTE 3", RPU_CPC_CHUTE_3_SELECTION_BYTE, 1, 0, 8, 4, 1, CPCLabels, false},   // 15
};
// The audits (the RPU's, in its EEPROM places): shown in the menu after the settings, and sent to the app
struct GameAudit { const char *name; unsigned short eeprom; };
const GameAudit Audits[] = {
  {"TOTAL    PLAYS", RPU_TOTAL_PLAYS_EEPROM_START_BYTE},       // 0
  {"COINS  CHUTE 1", RPU_CHUTE_1_COINS_START_BYTE},            // 1
  {"COINS  CHUTE 2", RPU_CHUTE_2_COINS_START_BYTE},            // 2
  {"COINS  CHUTE 3", RPU_CHUTE_3_COINS_START_BYTE},            // 3
  {"TOTAL  REPLAYS", RPU_TOTAL_REPLAYS_EEPROM_START_BYTE},     // 4
  {"HI SCORE  BEAT", RPU_TOTAL_HISCORE_BEATEN_START_BYTE},     // 5
};
#define NUM_AUDITS (sizeof(Audits) / sizeof(Audits[0]))
void AddToAudit(unsigned short eeprom, unsigned long n = 1) { RPU_WriteULToEEProm(eeprom, RPU_ReadULFromEEProm(eeprom, 0) + n); }
// The values go to the node first (an audit snapshot), so what they were is on record
void ClearAudits() {
  PinNodeCtrl_SendAuditsNow();
  for (byte i = 0; i < NUM_AUDITS; i++) RPU_WriteULToEEProm(Audits[i].eeprom, 0);
  PinNodeCtrl_AuditsChanged();
}
#define NUM_SETTINGS (sizeof(Settings) / sizeof(Settings[0]))
void ServiceOutputs();
void PlaySound(byte code);
void Queue(unsigned long delayMs, byte sound, byte coil);
extern unsigned long BallsNotFoundAt;
boolean AllBallsHome();
void BallsNotFound();
void EnterOperatorMenu();
void SlamTilt();
long SettingValue[NUM_SETTINGS];
// The settings as the game uses them (ReadStoredParameters)
boolean FreePlay = FREE_PLAY, MatchOn = true, SlingsOn = true;
byte BallsPerGame = 3, MaxCredits = 20, HighScoreReplays = 3, MaxTiltWarnings = 2, AwardCount = 3, SaverMode = SAVER_ONCE;
unsigned long Replay1 = 1500000, Replay2 = 3500000, ExtraBallTimerMs = 33650, SpecialTimerMs = 33000;
// Anti-Gravity completions for the Extra Ball light, the Extra Ball and the Special light (Bally's "Award
// Count": 0 is 6/7/8 ... 3, the factory's, 3/4/5)
#define AG_EXTRA_BALL_LIGHT   (6 - AwardCount)
#define AG_EXTRA_BALL         (7 - AwardCount)
#define AG_SPECIAL_LIGHT      (8 - AwardCount)
// The timers: 8, 16 or 32 s on the card. The bench's 32 s ran 33.65 s (the smash's Extra Ball, its 29) and
// 33 s (the Special); the shorter ones scaled the same (provisional). The Special's 3: to the end of the ball
void ApplyParameters() {
  FreePlay = SettingValue[0];
  BallsPerGame = SettingValue[1];
  MaxCredits = SettingValue[2];
  Replay1 = SettingValue[3];
  Replay2 = SettingValue[4];
  HighScoreReplays = SettingValue[5];
  MatchOn = SettingValue[6];
  MaxTiltWarnings = SettingValue[7];
  SlingsOn = SettingValue[8];
  AwardCount = SettingValue[9];
  ExtraBallTimerMs = 33650UL >> (2 - SettingValue[10]);
  SpecialTimerMs = SettingValue[11] == 3 ? 0xFFFFFFFFUL : 33000UL >> (2 - SettingValue[11]);
  SaverMode = SettingValue[12];
}
// A coin: its chute's audit, and credits by the chute's coins / credits (chutes with the same setting share
// their coins in progress, as the RPU's). Chute 1 is the left coin switch, 2 the middle, 3 the right (the
// Bally matrix places; provisional for this door)
byte ChuteCoinsInProgress[3];
void AddCredits(byte n) {
  Credits = (Credits + n > MaxCredits) ? (Credits < MaxCredits ? MaxCredits : Credits) : Credits + n;
  RPU_WriteByteToEEProm(RPU_CREDITS_EEPROM_BYTE, Credits);
}
void AddCoin(byte chute) {
  // The original: 127 with each coin, and coil 12 33 ms after it (bench: three coins a run)
  PlaySound(SND_COIN);
  Queue(33, 0, SOL_COIN_METER);
  static const unsigned short chuteAudit[3] = {RPU_CHUTE_1_COINS_START_BYTE, RPU_CHUTE_2_COINS_START_BYTE, RPU_CHUTE_3_COINS_START_BYTE};
  AddToAudit(chuteAudit[chute]);
  byte selection = SettingValue[13 + chute], first = 0;
  while (first < chute && SettingValue[13 + first] != selection) first++;
  byte coins = CPCPairs[selection][0], credits = CPCPairs[selection][1];
  byte before = ChuteCoinsInProgress[first]++;
  if (ChuteCoinsInProgress[first] >= coins) {
    AddCredits(credits > coins ? credits - before : credits);
    ChuteCoinsInProgress[first] = 0;
  } else if (credits > coins) {
    AddCredits(1);
  }
}
void WriteParameter(byte i) {
  if (Settings[i].size == 4) RPU_WriteULToEEProm(Settings[i].eeprom, SettingValue[i]);
  else RPU_WriteByteToEEProm(Settings[i].eeprom, (byte)SettingValue[i]);
}
void SetAllParameterDefaults() {
  for (byte i = 0; i < NUM_SETTINGS; i++) { SettingValue[i] = Settings[i].defaultValue; WriteParameter(i); }
  RPU_WriteByteToEEProm(EEPROM_SETTINGS_MARK_BYTE, SETTINGS_EEPROM_MARK);
}
// From EEPROM (at power-up, and after the app changes one): a value out of range takes its default
void ReadStoredParameters() {
  byte mark = RPU_ReadByteFromEEProm(EEPROM_SETTINGS_MARK_BYTE);
  if (mark == 0xB3) {   // written by a build with settings 1-13 only: the coin settings start at their defaults
    for (byte i = 13; i < NUM_SETTINGS; i++) { SettingValue[i] = Settings[i].defaultValue; WriteParameter(i); }
    RPU_WriteByteToEEProm(EEPROM_SETTINGS_MARK_BYTE, SETTINGS_EEPROM_MARK);
  } else if (mark != SETTINGS_EEPROM_MARK) SetAllParameterDefaults();
  for (byte i = 0; i < NUM_SETTINGS; i++) {
    long v = Settings[i].size == 4 ? (long)RPU_ReadULFromEEProm(Settings[i].eeprom, Settings[i].defaultValue)
                                   : (long)RPU_ReadByteFromEEProm(Settings[i].eeprom);
    if (v < Settings[i].minValue || v > Settings[i].maxValue || (v - Settings[i].minValue) % Settings[i].step) {
      v = Settings[i].defaultValue;
      SettingValue[i] = v;
      WriteParameter(i);
    }
    SettingValue[i] = v;
  }
  ApplyParameters();
}
#define AG_WINDOW_MS          2400     // Anti-Gravity entrance to exit (bench: 2.3 s counts, 2.5 s doesn't)
#define TIMED_END_MS          4000     // 29 this long after the timed Extra Ball is collected (bench)

// The player's, kept from ball to ball
byte Volts[4];
unsigned int ThumperHits[4];
unsigned long HeldBonus[4];             // Hold Bonus: the multiplied total, counted again after the player's next ball (bench)
byte PlayerStrange[4], PlayerScience[4], PlayerLab[4];   // kept from ball to ball (bench)
boolean PlayerSparkAtAG[4];
boolean Replay1Given[4], Replay2Given[4], HighScoreGiven[4];

// This ball
byte StrangeLetters = 0, ScienceLetters = 0, LabLetters = 0;
byte StrangeHeld = 0, ScienceHeld = 0;   // new letters that haven't started moving yet (see StepWord)
byte StrangeLatest = 0, ScienceLatest = 0;   // the word's last new (or toggled) letter while it's held (see LightLetter)
byte StrangeToggle = 0, ScienceToggle = 0;   // a toggled letter, until the step that ends its flash (see EndToggle)
unsigned long WordsFlashFrom = 0;    // both words flashing whole after a 111 (see WordsDone)
unsigned long LetterFlashFrom = 0;   // a new STRANGE / SCIENCE letter's flash (see RotationAfterHit)
unsigned long WordFlashFrom = 0;     // a completed word flashing whole, 50-470 ms after its hit (see LightLetter)
byte *WordFlashWord = 0;
// The letter targets' sounds held after a word's sound (see LetterSound)
unsigned long LetterSoundHoldUntil = 0, LetterSoundSlotAt = 0;
boolean LetterSoundSlots = false;
byte LetterHitsWaiting[8], LetterSoundsCount = 0;   // each waiting hit's target
boolean LetterHitsAgain[8];       // and whether it was the word's last target hit, as it was made
byte LetterHitsSteps[8];          // and its word's steps so far, as it was made
boolean LetterFromSlot = false, LetterSlotAgain = false;   // a held hit counting at its slot
byte LetterSlotSteps = 0, LastLetterSound = 0;
byte StrangeSteps = 0, ScienceSteps = 0;   // each word's rotation steps (any letter moved)
byte StrangeLastHit = 0xFF, ScienceLastHit = 0xFF;   // each word's last target hit, as made
byte *LetterFlashWord = 0, LetterFlashIndex = 0;
byte BonusK = 0, BonusMultiplier = 1;
boolean CollectBonusLit = false, HoldBonusLit = false, SaverLit = false, Tilted = false, BallConfirmed = false;
boolean ShootAgain = false, SpecialLit = false, SparkAtAntiGravity = false;
byte TiltWarnings = 0, AGCompletions = 0, SparkLevel = 0, BubbleIndex = 0, LabCompletions = 0;
unsigned long AntiGravityEnterTime = 0, LeftOutlaneTime = 0, BallStartTime = 0, AtomicFlashUntil = 0;
unsigned long LastTiltTime = 0, ExtraBallUntil = 0, SpecialUntil = 0, NextSweep = 0, NextBubble = 0;
unsigned long SparkCollectedAt = 0, NextMB80 = 0, NextMB72 = 0, MB72RunEnd = 0;
boolean MBLoopHeld = false;   // the 72/80 loop waits through a release made during multiball
unsigned long LoadServeAt = 0, ServeKickAt = 0, PinsAgainAt = 0, ReleaseShowFrom = 0, ReleaseShowUntil = 0;
byte LoadServeSound = 0;
boolean LoadServed = false;   // a load's serve: the drone starts again after its plunge (bench: five)
unsigned long VoltsFlashUntil = 0, ServeAt = 0;
#define SAVER_WINDOW_MS       4000   // after the left outlane: the ball is there to be flipped back
unsigned long SaverGoesOutAt = 0;
boolean SaverArmed = false;
boolean SlingsArmed = true;

// The Power Saver's coil on the left cabinet button, fired by RPU's interrupt like the slings, only while it's
// lit in play. Switched with interrupts held off: RPU's interrupt reads the table.
// The slings are left out with the Slingshot setting off (Bally's "Sling Shot" register)
struct PlayfieldAndCabinetSwitch SwitchTriggers[NUM_SWITCHES_WITH_SAVER];
void SetSaverArmed() {
  boolean armed = (MachineState == MACHINE_STATE_NORMAL_GAMEPLAY) && SaverLit && !Tilted;
  if (armed == SaverArmed && SlingsOn == SlingsArmed) return;
  SaverArmed = armed;
  SlingsArmed = SlingsOn;
  noInterrupts();   // RPU's interrupt reads the table
  byte n = 0;
  for (byte i = 0; i < NUM_SWITCHES_WITH_SAVER; i++) {
    const PlayfieldAndCabinetSwitch &t = SolenoidAssociatedSwitchesWithSaver[i];
    if (t.switchNum == SW_BUTTON_LEFT && !armed) continue;
    if ((t.switchNum == SW_SLING_LEFT || t.switchNum == SW_SLING_RIGHT) && !SlingsOn) continue;
    SwitchTriggers[n++] = t;
  }
  RPU_SetupGameSwitches(n, n, SwitchTriggers);
  interrupts();
}

// Ball handling
unsigned long OutholeClosedSince = 0, LastOutholeKick = 0, LastServe = 0, SaucerHoldUntil = 0;
unsigned long LastPlayfieldSwitch = 0;
byte ServeAttempts = 0;
boolean ServePending = false;

// Ball accounting: the balls home (the trough's steady count, and the outhole) and the balls locked (the
// Load and Unload lanes) are counted from switches; the rest are in play. TotalBalls is what was home and
// locked at the start of the game.
#define MAX_BALLS             5
#define TROUGH_STEADY_MS      500
byte TotalBalls = MAX_BALLS, BallsInPlay = 0, BallsLocked = 0;
unsigned long BubbleShowAt = 0, BubbleShowValue = 0;   // a bubble award's display (see ShowBubbleAward)
byte TroughRaw = 0, TroughSteady = 0;
unsigned long TroughChangedAt = 0;
// The atom smasher's pins (the trap): down until the first ball is loaded, then up to hold it and the ones
// after it. Nothing reports where they are, so the game keeps track and pulses the coil it wants.
boolean PinsUp = false;
// Five loaded: out one at a time (down for one, up, down ...), then they stay down. Atoms loaded at game over
// stay for the next game.
byte ReleaseStep = 0, ReleaseTries = 0;
unsigned long ReleaseNextAt = 0;
boolean ReleaseAllMultiball = false;   // from the release until one ball is left; loads roll through

// ------------------------------------------------------------------ helpers
void SetText(int display, const char *text) {
  char buf[8];
  strncpy(buf, text, 7);
  buf[7] = 0;
  RPU_SetDisplayText(display, buf);
}

void SetMachineState(int state) {
  MachineState = state;
  MachineStateTime = Now;
  Serial.printf("state %d\n", state);
}

byte TroughCount() {
  byte count = 0;
  for (byte sw = SW_TROUGH_5; sw <= SW_TROUGH_1; sw++)
    if (RPU_ReadSingleSwitchState(sw)) count++;
  return count;
}

void PlaySound(byte code) {
  RPU_PlaySound6803(code);
  Serial.printf("%lu snd %u\n", Now, code);   // every sound command, for the machine's console log
}
void Pulse(byte solenoid, byte halfCycles, boolean evenIfDisabled = false);

// Sounds and knocks in time: the start-up guitars, Anti-Gravity before a serve, replays, the match wheel
#define QUEUE_SIZE 48
struct Queued { unsigned long at; byte sound; byte coil; };
Queued SoundQueue[QUEUE_SIZE];
byte QueueCount = 0;
void Queue(unsigned long delayMs, byte sound, byte coil = 0) {
  if (QueueCount >= QUEUE_SIZE) return;
  SoundQueue[QueueCount++] = {Now + delayMs, sound, coil};
}
void ClearQueue() { QueueCount = 0; }
void UpdateQueue() {
  for (byte i = 0; i < QueueCount;) {
    if (Now < SoundQueue[i].at) { i++; continue; }
    if (SoundQueue[i].sound) PlaySound(SoundQueue[i].sound);
    if (SoundQueue[i].coil) Pulse(SoundQueue[i].coil, SoundQueue[i].coil == SOL_KNOCKER ? 8 : 4, true);
    SoundQueue[i] = SoundQueue[--QueueCount];
  }
}

// Replays (free play still counts them, up to 20): 124 with each knock, 0.2 s apart
void AwardReplays(byte count) {
  AddToAudit(RPU_TOTAL_REPLAYS_EEPROM_START_BYTE, count);
  for (byte i = 0; i < count; i++) {
    if (Credits < MaxCredits) Credits++;
    Queue(200UL * i, SND_REPLAY, SOL_KNOCKER);
  }
  RPU_WriteByteToEEProm(RPU_CREDITS_EEPROM_BYTE, Credits);
}

void CheckReplays() {
  unsigned long score = Scores[CurrentPlayer];
  if (score >= Replay1 && !Replay1Given[CurrentPlayer]) { Replay1Given[CurrentPlayer] = true; AwardReplays(1); }
  if (score >= Replay2 && !Replay2Given[CurrentPlayer]) { Replay2Given[CurrentPlayer] = true; AwardReplays(1); }
  // Past the high score to date, or past 10 million: 3 replays, the moment it happens (seen in game 4)
  if (!HighScoreGiven[CurrentPlayer] && (score > HighScoreToBeat || score > 10000000UL)) {
    HighScoreGiven[CurrentPlayer] = true;
    AddToAudit(RPU_TOTAL_HISCORE_BEATEN_START_BYTE);
    AwardReplays(HighScoreReplays);
  }
}

// Balls in play multiply playfield scores: 2 balls 3X, 3 6X, 4 9X, 5 12X
boolean InMultiball();
byte PlayMultiplier() {
  static const byte mult[6] = {1, 1, 3, 6, 9, 12};
  return InMultiball() ? mult[BallsInPlay > 5 ? 5 : BallsInPlay] : 1;
}

void AddScoreFlat(unsigned long points) {
  if (Tilted || MachineState != MACHINE_STATE_NORMAL_GAMEPLAY) return;
  Scores[CurrentPlayer] += points;
  CheckReplays();
}

void AddScore(unsigned long points) { AddScoreFlat(points * PlayMultiplier()); }

void AddBonus(byte k) {
  if (Tilted) return;
  BonusK += k;
  if (BonusK > 59) BonusK = 59;   // the lamps go to 59K
}

byte CurrentBackground = 0;
unsigned long DroneFrom = 0;
void UpdateBackground();

// Stop everything (the board layers sounds, and a background loops until stopped)
void StopSounds() {
  PlaySound(SND_STOP);
  CurrentBackground = 0;
}

void Pulse(byte solenoid, byte halfCycles, boolean evenIfDisabled) {
  RPU_PushToSolenoidStack(solenoid, halfCycles, evenIfDisabled);
}

// The outhole kicks its ball into the trough whatever the state (535 ms closed, so the coil at about 0.57 s as
// the original's; 1.5 s between kicks). A
// kicked ball counts as home until the trough's count goes up (3 s at most), so it isn't in play again
// for the moment it rolls.
#define OUTHOLE_KICK_MS 535
byte BallsToTrough = 0;
unsigned long LastToTrough = 0;
// The multiball's drone (26) in place of the volts one, and whether a ball has drained since it started
boolean MBDrone = false, MBDrained = false;
boolean InMultiball();
boolean OutholeKickedBall() {
  if (!RPU_ReadSingleSwitchState(SW_OUTHOLE)) { OutholeClosedSince = 0; return false; }
  if (!OutholeClosedSince) OutholeClosedSince = Now;
  if (Now - OutholeClosedSince > OUTHOLE_KICK_MS && Now - LastOutholeKick > 1500) {
    LastOutholeKick = Now;
    Pulse(SOL_OUTHOLE, 6, true);
    OutholeClosedSince = 0;
    BallsToTrough++;
    LastToTrough = Now;
    if (InMultiball()) MBDrained = true;
    return true;
  }
  return false;
}

// The saucer gives its ball back a second after it lands, in any state but diagnostics (a ball left there
// by a slam tilt, or there at power-up, too)
void SaucerKick() {
  if (MachineState == MACHINE_STATE_DIAGNOSTICS) return;
  if (!SaucerHoldUntil && RPU_ReadSingleSwitchState(SW_SAUCER)) SaucerHoldUntil = Now + 1000;
  if (SaucerHoldUntil && Now > SaucerHoldUntil) {
    SaucerHoldUntil = 0;
    if (RPU_ReadSingleSwitchState(SW_SAUCER)) Pulse(SOL_SAUCER, 6, true);
  }
}

// The ball counts home with the kick, so the drain's sounds run from it (bench: rmsg-p2, p3, hold2)
boolean OutholeHolding() { return OutholeClosedSince && Now - OutholeClosedSince > OUTHOLE_KICK_MS; }
// A kick that didn't get the ball out leaves it both kicked and in the outhole: it's one ball (a second
// drain behind a rolling one is seen once the trough settles)
byte BallsHome() {
  byte outhole = OutholeHolding() ? 1 : 0;
  return TroughSteady + (BallsToTrough > outhole ? BallsToTrough : outhole);
}

void LogBalls(const char *why) {
  Serial.printf("balls: %s (in play %d, locked %d, home %d, of %d)\n", why, BallsInPlay, BallsLocked,
                BallsHome(), TotalBalls);
}

// Every loop: the trough's count once it has held still for 500 ms (balls roll and bounce), then the balls
// in play. More home and locked than the game had means the counts were off: the switches win.
void UpdateBallCounts() {
  byte raw = TroughCount();
  if (raw != TroughRaw) { TroughRaw = raw; TroughChangedAt = Now; }
  else if (Now - TroughChangedAt >= TROUGH_STEADY_MS && TroughSteady != raw) {
    if (raw > TroughSteady) BallsToTrough = (raw - TroughSteady >= BallsToTrough) ? 0 : BallsToTrough - (raw - TroughSteady);
    TroughSteady = raw;
  }
  if (BallsToTrough && Now - LastToTrough > 3000) BallsToTrough = 0;
  byte home = BallsHome();
  if (home > TotalBalls) TotalBalls = home > MAX_BALLS ? MAX_BALLS : home;
  if (home + BallsLocked > TotalBalls) { BallsLocked = TotalBalls - home; LogBalls("lock count corrected"); }
  byte inPlay = TotalBalls - home - BallsLocked;
  if (inPlay != BallsInPlay) {
    BallsInPlay = inPlay;
    LogBalls("in play");
  }
}

// Serve a ball from the trough with the out kicker. It worked if the trough holds one ball fewer 1.5 s
// later (or a playfield switch has closed since the kick); if not, kick again (up to 4 times). Never a
// second ball while the first one sits in the shooter lane.
byte ServeTroughCount = 0;
void ServeBall() {
  if (!ServePending) return;
  if (LastServe && Now - LastServe < 1500) return;
  if (LastServe && (TroughSteady < ServeTroughCount || LastPlayfieldSwitch > LastServe)) {
    ServePending = false;
    return;
  }
  if (LastServe && Now - LastServe < 3000) return;
  // Only from a settled trough: a ball still rolling in would hide the one kicked out, and it'd be kicked twice
  if (BallsToTrough || OutholeClosedSince || TroughRaw != TroughSteady) return;
  if (!RPU_ReadSingleSwitchState(SW_TROUGH_1)) return;
  if (ServeAttempts >= 4) { ServePending = false; Serial.println("serve: gave up"); return; }
  ServeAttempts++;
  ServeTroughCount = TroughSteady;
  LastServe = Now;
  Pulse(SOL_OUT_KICKER, 8, true);
  Serial.printf("serve %d (trough %d)\n", ServeAttempts, ServeTroughCount);
}

void RequestServe() {
  ServePending = true;
  ServeAttempts = 0;
  LastServe = 0;
}

// ------------------------------------------------------------------ atom smasher
// The pins' coils, in half-cycles (8.3 ms each). 4 moved them in hardware's coil walk, with no balls on them;
// "pulse N" on USB tries another (not saved).
byte PinPulse = 4;
// The original pulses the pins' coil again 20 s after the last time it pulsed them (bench: hold2)
unsigned long NextPinRefresh = 20000, LastPinsMove = 0;
unsigned long FiveLoopAt = 0;   // the five-ball release's 72 runs (see StartMultiballSounds)
// At power-up the original pulses the pins down twice, 6.34 and 6.44 s in: anything trapped rolls out and
// drains to the trough (bench: held2p, five, p4)
unsigned long PowerUpPinsAt = 6340;
byte PowerUpPinsLeft = 2;

void RaisePins() {
  Pulse(SOL_CLOSE_ATOM_SMASHER, PinPulse, true);
  PinsUp = true;
  NextPinRefresh = Now + 20000;
  LastPinsMove = Now;
}

void LowerPins() {
  Pulse(SOL_OPEN_ATOM_SMASHER, PinPulse, true);
  PinsUp = false;
  NextPinRefresh = Now + 20000;
  LastPinsMove = Now;
}

// Let the trapped balls out one at a time (all at once, they'd just drain): pins down until a ball crosses the
// Unload lane (RELEASE_DOWN_MS at most), up again, RELEASE_GAP_MS, the next. As the original: one drop (and
// one 120) per ball locked, and the last drop leaves the pins down, so anything still on them rolls out
// (bench: qrel2, qrel4, five).
// It's the five-ball multiball (and "release" on USB).
// Set from USB for testing on the machine ("drop N", "gap N"); not saved
// The original drops them about 0.33 s apart: the pins open, close 0.125 s later, open again ...
unsigned int ReleaseDownMs = 125;
unsigned int ReleaseGapMs = 205;
#define RELEASE_DOWN_MS       ReleaseDownMs
#define RELEASE_GAP_MS        ReleaseGapMs
#define RELEASE_DROPPED       1   // ReleaseStep: the pins are down for one ball
#define RELEASE_WAITING       2   // ReleaseStep: the pins are up until the next drop
// Five loaded: with the first atom out the original pays the lit bubble value x12, its sound x3 from 183 ms,
// and lets the next atom out 983 ms after the first (bench: five; the original's atoms go 0.3 s apart after)
boolean ReleaseBubble = false;
unsigned long ReleaseHoldUntil = 0, ReleaseBubbleAt = 0;
void StartRelease(boolean multiball, unsigned long delayMs = 1000) {   // the last ball loaded comes to rest first
  ReleaseBubble = false;
  ReleaseHoldUntil = 0;
  ReleaseAllMultiball = multiball;
  ReleaseTries = BallsLocked;   // drops left
  ReleaseStep = RELEASE_WAITING;
  ReleaseNextAt = Now + delayMs;
  LogBalls(multiball ? "release: multiball" : "release");
}

void UpdateRelease() {
  if (ReleaseBubbleAt && Now >= ReleaseBubbleAt) {
    ReleaseBubbleAt = 0;
    unsigned long value = BubbleValues[BubbleIndex] * 12UL;
    AddScoreFlat(value);
    BubbleShowAt = Now;
    BubbleShowValue = value;
    for (byte i = 0; i < 3; i++) Queue(i ? 66 + 350UL * i : 0, SND_COIN_1 + BubbleIndex);   // 0, 417, 767
  }
  if (!ReleaseStep || Now < ReleaseNextAt) return;
  if (ReleaseStep == RELEASE_WAITING && Now < ReleaseHoldUntil) return;
  if (ReleaseStep == RELEASE_DROPPED) {   // no ball came out in time
    RaisePins();
    ReleaseStep = RELEASE_WAITING;
    ReleaseNextAt = Now + RELEASE_GAP_MS;
    return;
  }
  LowerPins();
  if (!BallsLocked) { ReleaseStep = 0; LogBalls("release: all out"); return; }
  if (ReleaseAllMultiball && MachineState == MACHINE_STATE_NORMAL_GAMEPLAY && !Tilted) {
    PlaySound(SND_ATOM_OUT);
    if (ReleaseBubble) { ReleaseBubble = false; ReleaseBubbleAt = Now + 183; ReleaseHoldUntil = Now + 983; }
  }
  if (ReleaseTries <= 1) {   // the last drop: the pins stay down, and what's left is taken as out
    BallsLocked = 0;
    ReleaseStep = 0;
    UpdateBallCounts();   // now, or this pass sees none in play and ends the ball (bench: five)
    if (MBLoopHeld) { MBLoopHeld = false; NextMB72 = Now + 50; }   // the 72s again 50 ms after (bench)
    LogBalls("release: last drop");
    return;
  }
  ReleaseTries--;
  ReleaseStep = RELEASE_DROPPED;
  ReleaseNextAt = Now + RELEASE_DOWN_MS;
}

// A ball crossed the Unload lane while the pins were down for it: up again for the rest, or done
void ReleasedOne() {
  if (!BallsLocked) { ReleaseStep = 0; LogBalls("release: all out"); return; }
  RaisePins();
  ReleaseStep = RELEASE_WAITING;
  ReleaseNextAt = Now + RELEASE_GAP_MS;
}

// Balls loaded now roll through and back out: the pins are down for a release
boolean SmasherReleasing() { return ReleaseStep || ReleaseAllMultiball; }
unsigned long ReleaseAward = 0, ReleaseAwardAt = 0;
unsigned long LabScoreAt = 0;   // L-A-B's 10K, scored as its message ends

// The game's reactions to the smasher (in play, not tilted)
void CollectSpark(const char *top, const char *bottom, boolean lampShow = true);
void SpotLetters(byte count);
void ShowMessage(const char *top, const char *bottom, unsigned long ms, unsigned long value = 0);
void StartMultiballSounds();
unsigned long SparkValue();
void StartMarqueeValue(const char *words, unsigned long value, unsigned long delayMs = 116);

// The upper flipper put a ball in through the rear entrance. The first one fires the pins up to trap it. Each
// load collects the Spark Bonus. Five loaded, or a load during multiball with balls still stored, releases
// them all; during that release (and the five-ball multiball after it) every load rolls straight through.
void BallLoaded() {
  if (SmasherReleasing()) { LogBalls("loaded: rolls through"); return; }
  boolean multiballBefore = InMultiball();
  // In multiball with none stored the pins stay down and the ball rolls straight through, with 120 (bench:
  // qrel1, qebcollect)
  if (multiballBefore && !BallsLocked) {
    LogBalls("loaded: rolls through (multiball, none stored)");
    if (MachineState == MACHINE_STATE_NORMAL_GAMEPLAY && !Tilted) Queue(355, SND_ATOM_OUT);
    return;
  }
  if (!PinsUp) { RaisePins(); PinsAgainAt = Now + 1300; }   // and again 1.3 s later (bench)
  BallsLocked++;
  UpdateBallCounts();
  LogBalls("locked");
  if (MachineState != MACHINE_STATE_NORMAL_GAMEPLAY || Tilted) return;   // after a tilt, the ball ends (see the play state)
  // 107 and 152 0.27 s after the switch (0.36 s in multiball), and ATOM LOADED with the value scrolls from
  // 0.1 s after them (bench)
  unsigned long fx = multiballBefore ? 363 : 270;
  Queue(fx, SND_BALL_LOCKED);
  Queue(fx + (multiballBefore ? 0 : 17), SND_EFFECT);
  unsigned long value = SparkValue();
  CollectSpark("ATOM", "LOADED");
  StartMarqueeValue("ATOM LOADED", value, fx + 100);
  SpotLetters(1);
  // The award is paid once, 30K for every ball after the first in the multiball it makes (stored plus in
  // play): five loaded 120K; during multiball 2 stored 60K, 3 90K, 4 120K (bench). Five loaded: 30 2.8 s after
  // the load (where the next serve would be), RELEASE ATOMS from 5.9 s, then ALL ATOMS RELEASED and the award
  // with 152 7.37 s after the 30; 69 12.6 s after the 30, 26 and the first atom out 0.55 s later, the atoms
  // out one at a time, 120 with each. A load during multiball with others stored: 120 three times from 1.9 s,
  // then 152, 69 and the award at 5.8 s, the atoms out from 0.55 s after the 69; the 72/80 loop waits (bench).
  if (BallsLocked >= TotalBalls || (multiballBefore && BallsLocked > 1)) {
    ReleaseAward = 30000UL * (BallsLocked + BallsInPlay - 1);
    if (BallsLocked >= TotalBalls) {
      Queue(2805, SND_MULTIBALL);
      PinsAgainAt = Now + 2626;   // the pins up again 184 ms before the 30 (bench: five)
      ReleaseShowFrom = Now + 5807;   // 3.0 s after the 30
      ReleaseShowUntil = ReleaseAwardAt = Now + 10172;
      Queue(15405, SND_MB_RELEASE_1);
      Queue(15954, SND_MB_RELEASE_2);
      MBDrone = true;
      MBDrained = false;
      StartRelease(true, 15955);
      ReleaseBubble = true;
      FiveLoopAt = Now + 15405 + 2550;
    } else {
      ShowMessage("RELEASE", "", 5800);
      for (byte i = 0; i < 3; i++) Queue(1900 + 500UL * i, SND_ATOM_OUT);
      ReleaseAwardAt = Now + 5812;
      Queue(5812, SND_MB_RELEASE_1);
      StartRelease(true, 6362);
      // The loop stops until the last atom is out (then 72 at once), and 80 until 12.27 s after the 69
      // (bench: qrel2, qrel3, qrel4)
      MBLoopHeld = true;
      NextMB80 = Now + 5812 + 12270;
    }
    return;
  }
  // The next ball 2.8 s later, with the serve music for the atoms held (bench)
  if (!BallsInPlay) {
    LoadServeAt = Now + 2805;
    LoadServeSound = BallsLocked >= 3 ? SND_SERVE_3 : SND_SERVE_1;
  }
}

// A ball out through the Unload lane: released, or the front trapped ball smashed out the back by the ball in
// play. A smash collects the Spark Bonus, starts multiball and lights the timed Extra Ball. The last one out
// lets the pins down for the next load.
void BallUnloaded() {
  if (!BallsLocked) { LogBalls("unload: none locked"); return; }
  BallsLocked--;
  UpdateBallCounts();
  LogBalls("out of the smasher");
  if (ReleaseStep == RELEASE_DROPPED) ReleasedOne();
  if (SmasherReleasing()) return;
  if (!BallsLocked) LowerPins();
  if (MachineState == MACHINE_STATE_NORMAL_GAMEPLAY && !Tilted && BallsInPlay >= 2) {
    PlaySound(SND_SMASH);   // the two-ball's background, in place of the volts drone
    MBDrone = true;
    PlaySound(SND_EFFECT);
    CollectSpark("ATOM", "UNLOADED");
    ExtraBallUntil = Now + ExtraBallTimerMs;
    SaverLit = true;   // the smash lights the Power Saver (bench: it flashes from the smash on)
    StartMultiballSounds();
  }
}

// ------------------------------------------------------------------ neon and backbox
// The original strobes the neon 33 ms on, 33 ms off for 3-10 s after a ball goes into play and through the
// end-of-ball bonus; it's on through a game and off in attract, where the backbox light flashes.
boolean NeonLit = true;
unsigned long NeonStrobeUntil = 0;

void SetNeon(boolean on) {
  if (on == NeonLit) return;
  NeonLit = on;
  RPU_SetContinuousSolenoidBit(!on, CONTSOL_6803_NEON_OFF);
}

void StrobeNeon(unsigned long ms) { NeonStrobeUntil = Now + ms; }

boolean BackboxLit = true;
void SetBackbox(boolean on) {
  if (on == BackboxLit) return;
  BackboxLit = on;
  RPU_SetContinuousSolenoidBit(!on, CONTSOL_6803_BACKBOX_OFF);
}

void UpdateNeonAndBackbox() {
  if (MachineState == MACHINE_STATE_DIAGNOSTICS) return;   // the coil test switches them itself
  if (Menus.OperatorMenusActive()) { ServiceOutputs(); return; }
  if (MachineState == MACHINE_STATE_ATTRACT) {
    SetNeon(false);
    SetBackbox((Now / 500) % 2);
    return;
  }
  SetBackbox(true);
  SetNeon(Now < NeonStrobeUntil ? (Now / 33) % 2 == 0 : true);
}

boolean InMultiball() { return ReleaseAllMultiball || BallsInPlay >= 2; }

// The drone for this volts level (16 up), changed when the level changes; none after a tilt. The original sends
// the new level's code 17 ms after the sound before it and never stops the old one: the board swaps the
// background itself (bench: every volts step, 48 then 17-19 17 ms later; 255 only at the game start). Sent
// with nothing between, stop, drone and thumper, the drone was lost on the machine (a3507e8, ball 3)
void UpdateBackground() {
  byte level = SND_DRONE_1 + Volts[CurrentPlayer];
  byte wanted = Tilted ? 0 : (level > SND_DRONE_LAST ? SND_DRONE_LAST : level);
  if (wanted == CurrentBackground) return;
  if (wanted) Queue(17, wanted);
  else PlaySound(SND_STOP);
  CurrentBackground = wanted;
}

// ------------------------------------------------------------------ displays
// A score on a 7-character display. Scores carry on past 9,999,999 (the original rolled over and lost a
// high score that way): from 10 million they show in thousands, "10177K".
void ShowScore(int display, unsigned long score, boolean flash = false) {
  if (score <= RPU_OS_MAX_DISPLAY_SCORE) {
    if (flash) RPU_SetDisplayFlash(display, score, Now, 250, 2);
    else RPU_SetDisplay(display, score, true, 2);
    return;
  }
  if (flash && (Now / 250) % 2) { RPU_SetDisplayBlank(display, 0); return; }
  char text[8];
  snprintf(text, sizeof(text), "%luK", score / 1000);
  SetText(display, text);
}

// A message for a moment on the two displays away from the player's score: words on top, and a word or a
// value below
const char *MessageTop = 0, *MessageBottom = 0;
unsigned long MessageUntil = 0, MessageValue = 0;
void ShowMessage(const char *top, const char *bottom, unsigned long ms, unsigned long value) {
  MessageTop = top;
  MessageBottom = bottom;
  MessageValue = value;
  MessageUntil = Now + ms;
}

// The game start's STRANGE SCIENCE build-in (bench timing): 0.23 s after Start, STRANGE on display 1 and
// SCIENCE on display 2 light a segment more of each letter a step, whole at 2.23 s; they hold to 3.3 s, then
// go out a step at a time by 3.96 s, in the game's own five frames (bench: q111a-d's frames, the same at the
// start). A 111 plays it too, from 84 ms after the 111, a frame every 500 ms and down 167 ms apart from 3.1 s,
// clear at 3.77 s, the score and BALL n dark (bench: q111a-d, read through the same display path as the RPU)
unsigned long BuildInStart = 0, WordsBuildInAt = 0;
const unsigned int BuildInSteps[10] = {0, 467, 1000, 1534, 2000, 3067, 3267, 3400, 3600, 3734};
const unsigned int WordsBuildInSteps[10] = {0, 500, 1000, 1500, 2000, 3016, 3183, 3349, 3516, 3683};
const unsigned int *BuildInAt = BuildInSteps;
const byte BuildInStage[10] = {1, 2, 3, 4, 5, 4, 3, 2, 1, 0};
const byte BuildInFrames[2][5][7] = {
  {{0x20, 0x00, 0x04, 0x40, 0x80, 0x02, 0x08}, {0x24, 0x01, 0x44, 0x50, 0x81, 0x0A, 0x28},
   {0x25, 0x01, 0xC4, 0x56, 0x81, 0x2A, 0x29}, {0x2D, 0x01, 0xC6, 0x57, 0x85, 0x6A, 0x69},
   {0x6D, 0x81, 0xC7, 0x77, 0x87, 0x6F, 0x79}},   // STRANGE
  {{0x20, 0x01, 0x00, 0x08, 0x80, 0x01, 0x08}, {0x24, 0x11, 0x08, 0x28, 0x81, 0x11, 0x28},
   {0x25, 0x11, 0x88, 0x29, 0x81, 0x11, 0x29}, {0x2D, 0x19, 0x88, 0x69, 0x85, 0x19, 0x69},
   {0x6D, 0x39, 0x89, 0x79, 0x87, 0x39, 0x79}}};  // SCIENCE

boolean ShowBuildIn() {
  if (WordsBuildInAt && Now >= WordsBuildInAt) { BuildInStart = WordsBuildInAt; BuildInAt = WordsBuildInSteps; WordsBuildInAt = 0; }
  if (!BuildInStart) return false;
  if (Now >= BuildInStart + BuildInAt[9]) { BuildInStart = 0; return false; }
  byte stage = 0;
  if (Now >= BuildInStart)
    for (byte i = 0; i < 10; i++) if (Now - BuildInStart >= BuildInAt[i]) stage = BuildInStage[i];
  RPU_SetDisplayBlank(0, 0);
  RPU_SetDisplayBlank(3, 0);
  for (byte place = 0; place < 7; place++) {
    RPU_SetDisplaySegments6803(1, place, stage ? BuildInFrames[0][stage - 1][place] : 0);
    RPU_SetDisplaySegments6803(2, place, stage ? BuildInFrames[1][stage - 1][place] : 0);
  }
  return true;
}

// A line of text scrolled right to left through all 28 places (displays 0-3 in order), a place every 100 ms,
// the displays otherwise dark, from about 0.1 s after the 152 until it's gone; the displays are as they were
// until then (bench: ANTI GRAVITY, PARTICLE SEPARATOR, ATOM LOADED, ALL ATOMS RELEASED, each with its value)
char MarqueeText[32];
unsigned long MarqueeAt = 0;
void StartMarquee(const char *text, unsigned long delayMs = 116) {
  strncpy(MarqueeText, text, sizeof(MarqueeText) - 1);
  MarqueeText[sizeof(MarqueeText) - 1] = 0;
  MarqueeAt = Now + delayMs;
  MessageUntil = 0;
}

// The words and the value, right-aligned in six places after a space: ANTI GRAVITY  10000 (bench)
void StartMarqueeValue(const char *words, unsigned long value, unsigned long delayMs) {
  char text[32];
  snprintf(text, sizeof(text), "%s %6lu", words, value);
  StartMarquee(text, delayMs);
}

// EXTRA BALL (bench, ladder3 AG4; t = the 123): from +135 for about 3.93 s, EXTRA right-aligned on display 1
// and BALL on display 2 from +200, the others dark; EXTRA goes out for 133 ms every 333 ms from +334
unsigned long ExtraBallShowAt = 0;
boolean ShowExtraBall() {
  if (!ExtraBallShowAt || Now < ExtraBallShowAt) return false;
  unsigned long t = Now - ExtraBallShowAt + 102;   // from the 123
  if (t >= 4035) { ExtraBallShowAt = 0; return false; }
  SetText(0, "");
  SetText(3, "");
  SetText(1, (t >= 234 && (t - 234) % 333 < 200) ? "" : "  EXTRA");
  SetText(2, t < 167 ? "" : "BALL");
  return true;
}

// A line of text at one step of its scroll through the 28 places (the four displays, left to right in one
// row): false once it's gone
boolean ShowScrollStep(const char *text, int step) {
  int length = strlen(text);
  if (step >= 28 + length - 1) return false;
  for (int place = 0; place < 28; place++) {
    int i = place - 27 + step;   // the text's character at this place
    byte glyph = (i >= 0 && i < length) ? RPU_GetGlyph6803(text[i]) : 0;
    RPU_SetDisplaySegments6803(place / 7, place % 7, glyph);
  }
  return true;
}

boolean ShowMarquee() {
  if (!MarqueeAt || Now < MarqueeAt) return false;
  if (!ShowScrollStep(MarqueeText, (Now - MarqueeAt) / 100)) { MarqueeAt = 0; return false; }
  return true;
}

// Before each serve: TOP FLIPPER / LOADS ATOMS across the four displays, on 67 ms, off 133 ms. Times read from the
// bench's frames are corrected for its capture: a frame ORs in the one before, so a lit period shows 67 ms
// longer and a dark one 67 ms shorter, a lighting 33 ms late and a darkening 100 ms late (hardware's traces)
unsigned long IntroWordsFrom = 0, IntroWordsUntil = 0;
unsigned long ServeSoundAt = 0;   // the serve's sound (93-95), for the waiting display
unsigned long ReplayMsgFrom = 0, ReplayMsgUntil = 0;   // the replay message (see StartBall)
boolean ReplayMsgAnother = false;

// The bonus counting into the score at a drain (bench: hold2, rmsg-b; t from the outhole kick). From 3.03 s
// (about when the count starts): the score on display 1, COLLECT and BONUS on displays 2 and 3, and on display
// 4 the running total, what's under 10K first, then 10K more 34 ms after each 89. 0.47 s after the 90 the words
// go and the total slides left a place at a time into the score (21 places in 0.53 s), which shows the new
// score from 1.0 s after the 90; displays 2-4 are dark after it. A bonus held from the last ball is counted
// the same way next, HELD in place of COLLECT, with its own 90 and slide. With Hold Bonus lit, 1.87 s after
// the last 90 BONUS / HELD and this ball's total show for 133 ms, then every display is dark until the intro.
// Provisional: the slide's exact steps, the time between the two counts, and the displays with more players.
unsigned long BonusShowFrom = 0, BonusShowTotal = 0, BonusShow90 = 0, BonusShowShift = 0, BonusBackAt = 0;
byte BonusMove = 0;   // places the drained player's score moves to display 1 for the count (7 a display)
unsigned long BonusHeldTotal = 0, BonusHeldFrom = 0, BonusHeld90 = 0, BonusHeldFirst89 = 0;
// Players 2-4 with a held bonus: back to their own display after their own count, and forward again for HELD
unsigned long BonusBack1At = 0, BonusFwd2At = 0;
boolean BonusHoldShow = false;

// The words go 0.47 s after the 90 and the total slides in over 0.53 s (bench: 22 drains, the original 600
// bench ms after the BONUS frame where ours was 733)
#define BONUS_WORDS_GO  467
#define BONUS_SLIDE_END (BONUS_WORDS_GO + 534)

// One count and its slide (t from the outhole kick); false outside it
boolean ShowBonusPhase(unsigned long t, unsigned long from, unsigned long first89, unsigned long step3,
                       unsigned long t90, unsigned long total, const char *word) {
  if (t < from || t >= t90 + BONUS_SLIDE_END) return false;
  char line[29], value[8];
  if (t < t90 + BONUS_WORDS_GO) {
    // It counts up 1K at a time with the bonus lamps (see BonusLampsShown), an 89 every 10K (bench: lab's
    // and rmsg-c's thousands digit caught at 8, 6, 8 ... between the 89s; the owner)
    unsigned long firstStep = first89 - 9 * step3 / 30, counted = 0;
    if (t >= firstStep) counted = (t - firstStep) * 30 / step3 + 1;
    if (counted > total / 1000) counted = total / 1000;
    ShowScore(0, Scores[CurrentPlayer]);
    SetText(1, word);
    SetText(2, "  BONUS");
    if (counted) snprintf(value, sizeof(value), "%7lu", 1000UL * counted);
    else strcpy(value, "   0000");
    SetText(3, value);
    return true;
  }
  byte shift = (t - t90 - BONUS_WORDS_GO) * 21 / 534;
  snprintf(line, sizeof(line), "%7lu%21s", Scores[CurrentPlayer], "");
  snprintf(value, sizeof(value), "%lu", total);
  int end = 27 - shift, length = strlen(value);
  for (int i = 0; i < length; i++) if (end - length + 1 + i >= 0) line[end - length + 1 + i] = value[i];
  for (byte place = 0; place < 28; place++) RPU_SetDisplaySegments6803(place / 7, place % 7, RPU_GetGlyph6803(line[place]));
  return true;
}

// The player's score alone, right-aligned to end at a place (0-27), every other place dark
void ShowScoreAt(int end) {
  char line[29], value[8];
  memset(line, ' ', 28);
  line[28] = 0;
  snprintf(value, sizeof(value), "%lu", Scores[CurrentPlayer]);
  int length = strlen(value);
  for (int i = 0; i < length; i++) if (end - length + 1 + i >= 0) line[end - length + 1 + i] = value[i];
  for (byte place = 0; place < 28; place++) RPU_SetDisplaySegments6803(place / 7, place % 7, RPU_GetGlyph6803(line[place]));
}

boolean ShowBonusCount() {
  if (!BonusShowFrom || Scores[CurrentPlayer] > 9999999UL) return false;
  unsigned long t = Now - BonusShowFrom;
  if (t < (BonusMove ? 2853 : 2790)) return false;
  // Players 2-4: the other displays go dark 134 ms before the move (bench: p4)
  if (BonusMove && t < 2987) {
    ShowScoreAt(7 * CurrentPlayer + 6);
    return true;
  }
  // Player 1: the other displays go dark 0.18 s before COLLECT BONUS (bench: rmsg-p2)
  if (!BonusMove && t < 2970 + BonusShowShift) {
    ShowScore(0, Scores[CurrentPlayer]);
    SetText(1, ""); SetText(2, ""); SetText(3, "");
    return true;
  }
  // Players 2-4: the score moves left a place with each 44 (33 ms apart) to display 1 first, and back with
  // each 91 after (bench: rmsg-p3)
  if (BonusMove && t < 2987 + 33UL * BonusMove + 20) {
    unsigned long k = (t - 2987) / 33 + 1;
    if (k > BonusMove) k = BonusMove;
    ShowScoreAt(7 * CurrentPlayer + 6 - k);
    return true;
  }
  if (BonusBack1At && t >= BonusBack1At && t < BonusFwd2At) {
    if (t >= BonusBack1At + 33UL * BonusMove) return false;
    ShowScoreAt(6 + (t - BonusBack1At) / 33 + 1);
    return true;
  }
  if (BonusFwd2At && t >= BonusFwd2At && t < BonusFwd2At + 33UL * BonusMove + 20) {
    unsigned long k = (t - BonusFwd2At) / 33 + 1;
    if (k > BonusMove) k = BonusMove;
    ShowScoreAt(7 * CurrentPlayer + 6 - k);
    return true;
  }
  if (BonusMove && t >= BonusBackAt) {
    if (t >= BonusBackAt + 33UL * BonusMove) return false;
    ShowScoreAt(6 + (t - BonusBackAt) / 33 + 1);
    return true;
  }
  if (t < 2970 + BonusShowShift) return false;   // bench: 3.51 s after the outhole closes
  if (ShowBonusPhase(t, 2970 + BonusShowShift, 3243 + BonusShowShift, 775, BonusShow90, BonusShowTotal, "COLLECT")) return true;
  if (BonusHeldTotal && ShowBonusPhase(t, BonusHeldFrom, BonusHeldFirst89, 775, BonusHeld90, BonusHeldTotal, "   HELD")) return true;
  unsigned long holdAt = (BonusHeldTotal ? BonusHeld90 : BonusShow90) + 1833;
  if (BonusHoldShow && t >= holdAt) {
    boolean on = t < holdAt + 67;
    char value[8];
    snprintf(value, sizeof(value), "%7lu", BonusShowTotal);
    if (on) ShowScore(0, Scores[CurrentPlayer]); else SetText(0, "");
    SetText(1, on ? "  BONUS" : "");   // right-aligned (bench: hold2)
    SetText(2, on ? "HELD" : "");
    SetText(3, on ? value : "");
    return true;
  }
  ShowScore(0, Scores[CurrentPlayer]);
  SetText(1, ""); SetText(2, ""); SetText(3, "");
  return true;
}

// A bubble award (bench: rmsg-b, qboth, lab; t from the switch): the displays go dark, the value comes up on
// displays 3, 2, 4 and 1 in turn, then BUBBLE replaces it on 2, 4, 1 and 3; from 0.87 s the scores are back,
// the player's counting up to the new score in fifths of the value 0.1 s apart (provisional: with more
// players, and over another display show)
const unsigned int BubbleValueAt[4] = {334, 154, 87, 267}, BubbleWordAt[4] = {635, 435, 701, 501};   // bench less 33
boolean ShowBubbleAward() {
  if (!BubbleShowAt || Now < BubbleShowAt) return false;   // dark at once (the bench shows it 0.1 s later)
  unsigned long t = Now - BubbleShowAt;
  if (t >= 835) return false;
  char value[8];
  snprintf(value, sizeof(value), "%7lu", BubbleShowValue);
  for (byte d = 0; d < 4; d++) SetText(d, t >= BubbleWordAt[d] ? " BUBBLE" : t >= BubbleValueAt[d] ? value : "");
  return true;
}

// A lower L-A-B lane's new letter counts into the score 1000 at a time, from 0.39 s and 117 ms apart (bench, 14 lanes:
// hold2; provisional: the steps were uneven in the A lane, and other scores may count the same way). A STRANGE /
// SCIENCE letter's 5K the same from 0.47 s (bench: wone, 4 isolated hits, first 1K shown 552-571 ms, the 5th
// 952-1037). A later count-up takes over what's left, so a hit pauses one (words2, wsci). With 10K or more
// left it counts 10K a step (rmsg-c and rmsg-c2 after a 111, the queued hits' 5Ks: 10K steps, then 1K once
// under 10K; provisional: the step time, 67-133 ms there, kept at 117)
unsigned long LaneCountAt = 0, LaneCountValue = 0, LaneCountDelay = 388;
unsigned long LaneCountLeft() {
  if (!LaneCountAt) return 0;
  unsigned long steps = Now < LaneCountAt + LaneCountDelay ? 0 : (Now - LaneCountAt - LaneCountDelay) / 117 + 1;
  unsigned long tens = LaneCountValue / 10000;
  unsigned long counted = steps <= tens ? 10000UL * steps : 10000UL * tens + 1000UL * (steps - tens);
  if (counted >= LaneCountValue) { LaneCountAt = 0; return 0; }
  return LaneCountValue - counted;
}
void CountUp(unsigned long value, unsigned long delay) {
  LaneCountValue = value + LaneCountLeft();
  LaneCountAt = Now;
  LaneCountDelay = delay;
}

// The score as the bubble award's and the lanes' count-ups show it
unsigned long ShownCountedScore(byte p) {
  if (p == CurrentPlayer && LaneCountAt) {
    unsigned long left = LaneCountLeft();
    if (Scores[p] >= left) return Scores[p] - left;
  }
  if (p != CurrentPlayer || !BubbleShowAt || Now < BubbleShowAt + 835 || Scores[p] < BubbleShowValue) return Scores[p];
  // 10K at a time whatever the multiplier, 0.1 s apart (bench: rmsg-b's 50K, bubx12's 60K)
  unsigned long counted = 10000UL * ((Now - BubbleShowAt - 835) / 100 + 1);
  if (counted >= BubbleShowValue) { BubbleShowAt = 0; return Scores[p]; }
  return Scores[p] - BubbleShowValue + counted;
}
// After a 111 the score shows without the 70K until 0.2 s after the build-in, then with it at once, and BALL n
// comes back 0.1 s after that (bench: q111a-d, the old score from 111 +3.80-3.85 s, the new from +3.97-4.02,
// BALL 1 at +4.07-4.12)
unsigned long WordsAwardAt = 0, WordsAwardValue = 0;
unsigned long ShownScore(byte p) {
  unsigned long score = ShownCountedScore(p);
  if (p == CurrentPlayer && WordsAwardAt && Now < WordsAwardAt && score >= WordsAwardValue) score -= WordsAwardValue;
  return score;
}

// L-A-B completed (bench: hold2, lab, qlab1d-3d, qlab5): ADVANCE / BONUS / MULTI / PLIER from just after the
// first of the 72s for 1.46 s (its last beat dark), in 200 ms beats: in the first four, letters drop out at random for 100 ms; in
// the rest every display goes dark 100 ms. The 10K then shows in the score (provisional: the beats' exact
// phase, and the bench can't tell 67 from 133 ms)
unsigned long AdvanceShowAt = 0, TimesShowFrom = 0;
boolean ShowAdvanceBonus() {
  if (!AdvanceShowAt || Now < AdvanceShowAt) return false;
  unsigned long t = Now - AdvanceShowAt;
  if (t >= 1460) { AdvanceShowAt = 0; return false; }
  if (t >= 1400) { for (byte d = 0; d < 4; d++) SetText(d, ""); return true; }   // dark until the 10K shows
  const char *words[4] = {"ADVANCE", "  BONUS", "  MULTI", "PLIER  "};
  unsigned long beat = t / 200, phase = t % 200;
  for (byte d = 0; d < 4; d++) {
    char text[8];
    strcpy(text, words[d]);
    if (beat >= 4 && phase < 100) strcpy(text, "");
    else if (phase >= 100 && beat < 4) for (byte q = 0; q < 7; q++) if ((beat * 5 + d * 3 + q * 7) % 11 < 6) text[q] = ' ';   // made up
    SetText(d, text);
  }
  return true;
}

void ShowScores() {
  if (ShowBuildIn()) return;
  if (MachineState == MACHINE_STATE_COUNTDOWN_BONUS && ShowBonusCount()) return;
  if (ShowExtraBall()) return;
  if (ShowAdvanceBonus()) return;
  if (ShowBubbleAward()) return;
  if (ShowMarquee()) return;
  // Five loaded (bench: five; t from the first RELEASE): five cycles from 0, 0.73, 1.53, 2.93 and 3.67 s. In
  // each, RELEASE comes up on displays 3, 2, 4 and 1 (0, 133, 200, 267 ms), ATOMS replaces it on 2, 4, 1 and 3
  // (367, 433, 567, 633), and all are dark at 733. Between the 3rd and 4th, and after the 5th, the player's
  // score alone (ATOMS one place in, as the bench shows; provisional: the words fly in from the right too fast for the bench)
  if (Now >= ReleaseShowFrom && Now < ReleaseShowUntil) {
    const unsigned int cycleAt[5] = {0, 733, 1533, 2932, 3666};
    const unsigned int releaseAt[4] = {267, 133, 0, 200}, atomsAt[4] = {567, 367, 633, 433};
    unsigned long t = Now - ReleaseShowFrom;
    int cycle = -1;
    for (byte i = 0; i < 5; i++) if (t >= cycleAt[i] && t < cycleAt[i] + 733UL) cycle = i;
    for (byte d = 0; d < 4; d++) {
      if (cycle < 0) {
        if (d == CurrentPlayer && (t < 2865 || t >= 4399)) ShowScore(d, Scores[d]);
        else RPU_SetDisplayBlank(d, 0);
      } else {
        unsigned long c = t - cycleAt[cycle];
        SetText(d, c >= atomsAt[d] ? " ATOMS" : c >= releaseAt[d] ? "RELEASE" : "");
      }
    }
    return;
  }
  if (Now >= ReplayMsgFrom && Now < ReplayMsgUntil) {
    // Steady for 267 ms, then off 133 / on 67 (bench: rmsg-b, rmsg-p2, read through the bench's capture, which
    // shows a lit period 67 ms longer and a dark one 67 ms shorter; hold2's steady run looked longer)
    unsigned long t = Now - ReplayMsgFrom;
    boolean on = t < 267 || (t - 267) % 200 >= 133;
    SetText(0, on ? (ReplayMsgAnother ? "ANOTHER" : "  FIRST") : "");
    SetText(1, on ? "REPLAY" : "");
    SetText(2, on ? "   AT" : "");
    char value[8];
    snprintf(value, sizeof(value), "%lu", ReplayMsgAnother ? Replay2 : Replay1);
    SetText(3, on ? value : "");
    return;
  }
  // The playfield multiplied: THREE (SIX, NINE, TWELVE) TIMES / ALL SCORES flashing, from the first 72 run
  // after it lights, while it lasts (bench: bubx3, bubx12; provisional: SIX and NINE, and the on time)
  if (TimesShowFrom && PlayMultiplier() == 1) TimesShowFrom = 0;
  if (TimesShowFrom && Now >= TimesShowFrom) {
    static const char *const times[4] = {"THREE", "SIX", "NINE", "TWELVE"};
    boolean on = (Now - TimesShowFrom) % 200 < 100;
    SetText(0, on ? times[PlayMultiplier() / 3 - 1] : "");
    SetText(1, on ? "TIMES" : "");
    SetText(2, on ? "  ALL" : "");
    SetText(3, on ? "SCORES" : "");
    return;
  }
  if (Now >= IntroWordsFrom && Now < IntroWordsUntil) {
    boolean on = (Now - IntroWordsFrom) % 200 < 100;   // the bench shows 134 on: between 67 and 133 (provisional)
    SetText(0, on ? "  TOP" : "");
    SetText(1, on ? "FLIPPER" : "");
    SetText(2, on ? " LOADS" : "");
    SetText(3, on ? " ATOMS" : "");
    return;
  }
  for (byte p = 0; p < 4; p++) {
    if (p < NumPlayers) {
      // Waiting for the plunge: the score, then from 1.99 s after the serve sound dark 267 ms, BALL n 800,
      // dark 267, the score 800, and round again (bench: rmsg-p2, p3, hold2, at the start and every drain)
      unsigned long w = (ServeSoundAt && Now >= ServeSoundAt + 1990) ? (Now - ServeSoundAt - 1990) % 2134 : 2133;
      if (p == CurrentPlayer && !BallConfirmed && MachineState == MACHINE_STATE_NORMAL_GAMEPLAY && w < 1334) {
        if (w < 267 || w >= 1067) RPU_SetDisplayBlank(p, 0);
        else { char text[8]; snprintf(text, sizeof(text), " BALL %d", CurrentBall); SetText(p, text); }
      } else {
        ShowScore(p, ShownScore(p));
      }
    } else if (p == 3 && (Tilted || (BallConfirmed && !(BubbleShowAt && Now < BubbleShowAt + 835) &&
                                     !(WordsAwardAt && Now < WordsAwardAt + 100) &&
                                     !(MachineState == MACHINE_STATE_COUNTDOWN_BONUS && Now - MachineStateTime >= 885)))) {
      // BALL n once the ball is in play and a bubble award's display is over, until 1.42 s after the outhole
      // closes (bench: hold2, rmsg-p2, rmsg-f)
      char text[8];
      if (Tilted) snprintf(text, sizeof(text), "TILT");
      else snprintf(text, sizeof(text), " BALL %d", CurrentBall);   // right-aligned (bench)
      SetText(3, text);
    } else {
      RPU_SetDisplayBlank(p, 0);
    }
  }
  // Four players: the current one's display says the ball number for the first 2 s
  if (NumPlayers == 4 && Now - BallStartTime < 2000) {
    char text[8];
    snprintf(text, sizeof(text), "BALL %d", CurrentBall);
    SetText(CurrentPlayer, text);
  }
  if (Now < MessageUntil) {
    byte top = CurrentPlayer < 2 ? 2 : 0;
    SetText(top, MessageTop ? MessageTop : "");
    if (MessageBottom) SetText(top + 1, MessageBottom);
    else ShowScore(top + 1, MessageValue);
  }
}

// Attract (bench: rmsg-p2, rmsg-f; t from entering attract, when every display goes dark): from 0.87 s five
// messages 3.73 s apart, each sliding in from the right a place every 67 ms (all four displays at once, each
// within itself) and dark 2.93 s after it starts: GAME OVER, 1-4 CAN PLAY / CREDITS, FIRST REPLAY AT, SECOND
// REPLAY AT, BEAT (the high score) FOR 3 REPLAYS. In place of BEAT's dark the last scores (00 with no player)
// show at once for 4.13 s, dark 0.87 s, and round again (22.87 s)
#define ATTRACT_CYCLE_MS 22868
void ShowAttractDisplays() {
  unsigned long a = Now - MachineStateTime;
  if (a < 867) { for (byte d = 0; d < 4; d++) RPU_SetDisplayBlank(d, 0); return; }
  unsigned long t = (a - 867) % ATTRACT_CYCLE_MS;
  byte message = t * 2 / 7467;   // 3733.5 ms each
  unsigned long m = t - 7467UL * message / 2;
  if (message >= 4 && t >= 14934 + 2933) {
    for (byte p = 0; p < 4; p++) {
      if (t < 22001) ShowScore(p, p < LastNumPlayers ? LastScores[p] : 0);
      else RPU_SetDisplayBlank(p, 0);
    }
  } else if (m >= 2933) {
    for (byte d = 0; d < 4; d++) RPU_SetDisplayBlank(d, 0);
  } else {
    char text[4][8];
    switch (message) {
      case 0: strcpy(text[0], ""); strcpy(text[1], "   GAME"); strcpy(text[2], "OVER"); strcpy(text[3], ""); break;
      case 1: strcpy(text[0], "1-4 CAN"); strcpy(text[1], "PLAY"); strcpy(text[2], "CREDITS");
              snprintf(text[3], 8, "%7s", ""); snprintf(text[3] + 5, 3, "%02d", Credits % 100); break;
      case 2: strcpy(text[0], " FIRST"); strcpy(text[1], " REPLAY"); strcpy(text[2], "   AT");
              snprintf(text[3], 8, "%7lu", Replay1); break;
      case 3: strcpy(text[0], " SECOND"); strcpy(text[1], " REPLAY"); strcpy(text[2], "   AT");
              snprintf(text[3], 8, "%7lu", Replay2); break;
      default: strcpy(text[0], "  BEAT");
               if (HighScore > 9999999UL) snprintf(text[1], 8, "%6luK", HighScore / 1000);   // as ShowScore: thousands
               else snprintf(text[1], 8, "%7lu", HighScore);
               strcpy(text[2], " FOR  3"); strcpy(text[3], "REPLAYS"); break;
    }
    byte k = m < 400 ? 6 - m / 67 : 0;   // places still to slide
    for (byte d = 0; d < 4; d++) {
      char shown[8];
      memset(shown, ' ', 7);
      shown[7] = 0;
      for (byte q = k; q < 7; q++) if (q - k < (int)strlen(text[d])) shown[q] = text[d][q - k];
      SetText(d, shown);
    }
  }
  // Atoms kept for the next game: n ATOMS LOADED in the gaps between messages (where the original says it
  // isn't known; provisional)
  if (BallsLocked && a >= 867 && message < 4 && m >= 2933) {
    char text[8];
    snprintf(text, sizeof(text), "%d ATOMS", BallsLocked);
    SetText(2, BallsLocked == 1 ? "1 ATOM" : text);
    SetText(3, "LOADED");
  }
}

// ------------------------------------------------------------------ lamps
// After an Anti-Gravity completion (bench, hardware's comparison; t = the 152):
// - A.G. ENTRANCE and A.G. EXIT pulse 50 ms each, entrance, exit, entrance, exit, every 500 ms: 5 times from
//   0.2 s after a plain completion, 11 times from 0.35 s after a Spark Bonus one. After a spark one the
//   value lamp collected flashes once in each too.
// - At the exit, BACK LEFT pulses twice (plain) or A.G. EXIT once (spark).
// - About 0.93 s after the last, the backbox sweeps LEFT, MIDDLE, RIGHT, each twice, 83 ms apart.
// - The bubble lamps and the Particle Separator and Matter Transformer go dark for 2.5 s (the bubble's
//   value keeps stepping unseen, and comes back where it got to); after a spark one the separator's flashers
//   stay dark until the sweep.
unsigned long AGShowAt = 0, AGDarkUntil = 0;
// Before each serve the four bubble lamps flash together (250 ms on, 250 off) for 2 s from 1.18 s before the
// serve sound; then the value lamps step from 10K, 20K 0.88 s after the serve sound (bench: lab, hold2)
unsigned long BubbleFlashFrom = 0;
// A bubble hit holds the value 2.8 s (bench: the original steps 2.85 s after every hit). After the left lane
// (not the shooter lane's), the held lamp flashes from 0.83 s, 67 ms off and 67 on, until the hold ends.
#define BUBBLE_HOLD_MS 2805
unsigned long BubbleHeldAt = 0, BubbleHoldEnd = 0;
boolean BubbleHeldFlash = false;
boolean AGShowSpark = false;
byte AGShowLevel = 0;

void StartAGShow(boolean spark) {
  AGShowAt = Now ? Now : 1;
  AGShowSpark = spark;
  AGShowLevel = SparkLevel;
  AGDarkUntil = Now + 2500;   // the bubble's lamps only: its value keeps stepping (bench: bubdark2, 9 hits)
}

boolean Pulse50(unsigned long t, unsigned long at) { return t >= at && t < at + 50; }

// Sets the show's lamps; false once it's over (and the play lamps have them again)
boolean AGShowLamps() {
  if (!AGShowAt) return false;
  unsigned long t = Now - AGShowAt;
  unsigned long first = AGShowSpark ? 350 : 200, periods = AGShowSpark ? 11 : 5;
  unsigned long sweep = first + 500 * (periods - 1) + 930;
  if (t >= sweep + 470) { AGShowAt = 0; return false; }
  boolean entrance = false, exitLamp = false, value = false;
  if (t >= first && t < first + 500 * periods) {
    unsigned long p = (t - first) % 500;
    entrance = Pulse50(p, 0) || Pulse50(p, 167);
    exitLamp = Pulse50(p, 100) || Pulse50(p, 267);
    value = AGShowSpark && Pulse50(p, 352);
  }
  if (AGShowSpark) exitLamp = exitLamp || (t >= 40 && t < 85);
  RPU_SetLampState(LAMP_AG_ENTRANCE, entrance);
  RPU_SetLampState(LAMP_AG_EXIT, exitLamp);
  for (byte i = 0; i < 3; i++) RPU_SetLampState(LampsSpark[i], value && AGShowLevel == i);
  unsigned long s = t - sweep;
  RPU_SetLampState(LAMP_BACK_LEFT, (!AGShowSpark && (Pulse50(t, 0) || Pulse50(t, 80))) ||
                                   (t >= sweep && (Pulse50(s, 0) || Pulse50(s, 83))));
  RPU_SetLampState(LAMP_BACK_MIDDLE, t >= sweep && (Pulse50(s, 167) || Pulse50(s, 250)));
  RPU_SetLampState(LAMP_BACK_RIGHT, t >= sweep && (Pulse50(s, 334) || Pulse50(s, 417)));
  if (AGShowSpark && t < sweep) {
    RPU_SetLampState(LAMP_PARTICLE_LEFT, false);
    RPU_SetLampState(LAMP_PARTICLE_RIGHT, false);
  }
  return true;
}

// The bonus count (bench: lab, hold2, rmsg-p2; t from the drain's state, the 113 at 0.51 s): the bonus lamps
// go out at 0.90 s and every one of them flashes three times, 0.32 s on from 1.11 s, 0.63 s apart; the bonus
// shows again at 2.80 s, and from 9 steps before the first 89 counts down 1K every 25.8 ms (an 89 every 10K),
// once for each multiplier step, dark at the end of each. Provisional: the held bonus's count
byte BonusLampsShown() {
  byte shown = BonusK;
  if (MachineState == MACHINE_STATE_COUNTDOWN_BONUS && BonusShowFrom) {
    unsigned long s = Now - MachineStateTime, first = 3011 + BonusShowShift;
    if (s >= first && BonusK) {
      unsigned long steps = BonusShowTotal / 1000, j = (s - first) * 30 / 775 + 1;
      if (j >= steps) shown = 0;
      else shown = (j % BonusK) ? BonusK - j % BonusK : 0;
    }
  }
  return shown;
}

// --- event shows: on the RPU's lamp overlays (RPU 2026-10-17), over the lamps as their features set them, then
// gone. A lamp is forced on or off for the show; the rest show as they are. (The flashers ignore the overlays:
// they only strobe.)
#define SHOW_NONE 0
#define SHOW_OFF  1
#define SHOW_ON   2
// The bonus lamps at a drain: out 0.42 s after the 113, then every one of them flashing three times, 0.32 s on
// from 0.6 s, 0.63 s apart, until the bonus shows again at 2.3 s (bench: lab, hold2, rmsg-p2; t from the
// drain's state, the 113 at 0.51 s)
byte BonusFlashShow() {
  if (MachineState != MACHINE_STATE_COUNTDOWN_BONUS || !BonusShowFrom) return SHOW_NONE;
  unsigned long s = Now - MachineStateTime;
  if (s < 898 || s >= 2795) return SHOW_NONE;
  for (byte k = 0; k < 3; k++) if (s >= 1110 + 634UL * k && s < 1426 + 634UL * k) return SHOW_ON;
  return SHOW_OFF;
}
// STRANGE (word 0) and SCIENCE (word 1). After a 111 the words flash whole in turn, 334 ms each, STRANGE first
// from 34 ms after the 111 (470 ms after the hit; STRANGE alone from the 111, q111a/b) and STRANGE last, all dark
// from 3040 ms after the 111; quickly to 470 ms after the hit (bench: q111a/b with both words logged together;
// rmsg-c, rmsg-c2. Provisional: the quick part). A finished word flashes whole from 50 to 470 ms after its hit,
// dark over the other (bench: rmsg-c; provisional)
byte WordFlashShow(byte word) {
  byte show = SHOW_NONE;
  unsigned long words = Now - WordsFlashFrom;
  if (WordsFlashFrom && words < 3510) {
    boolean strange = words < 50 || (words >= 470 ? words < 504 || (words - 504) / 334 % 2 == 0 : (words - 50) % 84 >= 42);
    boolean science = words < 470 ? strange : words >= 504 && (words - 504) / 334 % 2 == 1;
    show = (word ? science : strange) ? SHOW_ON : SHOW_OFF;
  }
  byte *finished = word ? &ScienceLetters : &StrangeLetters;
  if (WordFlashWord == finished && Now - WordFlashFrom >= 50 && Now - WordFlashFrom < 470 && (Now - WordFlashFrom - 50) % 84 < 42)
    show = SHOW_OFF;
  return show;
}
void AddLampsToShow(byte show, const byte *lamps, byte count) {
  if (show == SHOW_NONE) return;
  for (byte i = 0; i < count; i++) RPU_AddLampToOverlay(show == SHOW_ON, lamps[i]);
}
// The overlays afresh each pass, with RPU's lamp interrupt held off so it never sees them half made
void UpdateEventShows() {
  boolean inPlay = MachineState == MACHINE_STATE_NORMAL_GAMEPLAY || MachineState == MACHINE_STATE_COUNTDOWN_BONUS;
  byte bonus = inPlay ? BonusFlashShow() : SHOW_NONE;
  byte strange = inPlay ? WordFlashShow(0) : SHOW_NONE, science = inPlay ? WordFlashShow(1) : SHOW_NONE;
  noInterrupts();
  RPU_ClearLampsOnOverlay();
  RPU_ClearLampsOffOverlay();
  AddLampsToShow(bonus, LampsBonusUnits, 9);
  AddLampsToShow(bonus, LampsBonusTens, 5);
  AddLampsToShow(strange, LampsStrange, 7);
  AddLampsToShow(science, LampsScience, 7);
  if (bonus || strange || science) {
    RPU_ApplyLampsOnOverlay();
    RPU_ApplyLampsOffOverlay();
  }
  interrupts();
}

// --- the play lamps, a function a feature

// STRANGE and SCIENCE
void ShowWordLamps() {
  // A new letter flashes until the rotation moves on (bench: wone, the lamp every 42 ms)
  // A letter a hit toggles off flashes the same way, then is dark (words2)
  boolean flashOn = (Now - LetterFlashFrom) % 84 < 42;
  // (Both words' flash after a 111, and a finished word's, are event shows on the overlays: see UpdateEventShows)
  for (byte i = 0; i < 7; i++) {
    boolean st = (StrangeLetters >> i) & 1, sc = (ScienceLetters >> i) & 1;
    if (LetterFlashWord && i == LetterFlashIndex) {
      if (LetterFlashWord == &StrangeLetters) st = flashOn; else sc = flashOn;
    }
    RPU_SetLampState(LampsStrange[i], st);
    RPU_SetLampState(LampsScience[i], sc);
  }
}

// The volts: 100V lit from the start, a lamp a step
void ShowVoltsLamps() {
  for (byte i = 0; i < 9; i++) RPU_SetLampState(LampsVolts[i], i <= Volts[CurrentPlayer]);   // 0 = 100V
}

// The bonus (counting down in the bonus count, see BonusLampsShown), and its multiplier
void ShowBonusLamps() {
  byte shown = BonusLampsShown();
  byte units = shown % 10, tens = shown / 10;
  for (byte i = 0; i < 9; i++) RPU_SetLampState(LampsBonusUnits[i], units == i + 1);
  // The tens lamps build up: 30K lights 10K, 20K and 30K (bench: rmsg-c's 59K, all five and 9K; lab's 24K)
  for (byte i = 0; i < 5; i++) RPU_SetLampState(LampsBonusTens[i], tens >= i + 1);
  for (byte i = 0; i < 5; i++) RPU_SetLampState(LampsBonusMult[i], i < LabCompletions);
}

// The playfield multiplier in multiball: 3X, 6X, 9X or 12X
void ShowPlayMultLamps() {
  byte mult = PlayMultiplier();
  for (byte i = 0; i < 4; i++) RPU_SetLampState(LampsPlayMult[i], mult == 3 * (i + 1));
}

// L-A-B
void ShowLabLamps() {
  RPU_SetLampState(LAMP_LAB_L, LabLetters & 1);
  RPU_SetLampState(LAMP_LAB_A, LabLetters & 2);
  RPU_SetLampState(LAMP_LAB_B, LabLetters & 4);
}

// The bubble lanes: the value cycling; all flashing at a serve; the left lane's hold flashing
void ShowBubbleLamps(boolean dark) {
  boolean serving = Now < BubbleFlashFrom + 2000;
  boolean flashAll = Now >= BubbleFlashFrom && (Now - BubbleFlashFrom) % 500 < 250;
  boolean heldOff = BubbleHeldFlash && Now < BubbleHoldEnd && Now >= BubbleHeldAt + 830 &&
                    (Now - BubbleHeldAt - 830) * 3 % 400 < 200;   // 67 off, 67 on
  for (byte i = 0; i < 4; i++) {
    boolean on = serving ? flashAll : (!dark && !heldOff && BubbleIndex == i);
    RPU_SetLampState(LampsBubbleLeft[i], on);
    RPU_SetLampState(LampsBubbleRight[i], on);
  }
}

// Collect Bonus, Hold Bonus, the Power Saver, Special, Extra Ball, Shoot Again
void ShowAwardLamps(boolean blink, boolean slow) {
  RPU_SetLampState(LAMP_COLLECT_BONUS, CollectBonusLit);
  RPU_SetLampState(LAMP_HOLD_BONUS, HoldBonusLit);
  // The Power Saver flashes while lit, quicker in its 4 s after the left outlane (bench)
  RPU_SetLampState(LAMP_SAVER_ON, SaverLit && ((LeftOutlaneTime && Now - LeftOutlaneTime < SAVER_WINDOW_MS) ? blink : (Now / 333) % 2));
  RPU_SetLampState(LAMP_SPECIAL, SpecialLit || (Now < SpecialUntil && slow));
  RPU_SetLampState(LAMP_EXTRA_BALL, (AGCompletions + 1 == AG_EXTRA_BALL) || (Now < ExtraBallUntil && slow));
  RPU_SetLampState(LAMP_SHOOT_AGAIN, ShootAgain);
}

// The Spark Bonus: at the Particle Separator or at Anti-Gravity, whichever is next
void ShowSparkLamps(boolean dark, boolean blink, boolean slow) {
  // (flashing)
  RPU_SetLampState(LAMP_PARTICLE_SEP, !SparkAtAntiGravity && !dark && slow);
  // Flashers only ever strobe, 50 ms at a time (see Flashers): the Particle Separator's right one every 1.17 s
  // and the left 67 ms after it (bench: rmsg-c)
  RPU_SetLampState(LAMP_PARTICLE_RIGHT, !SparkAtAntiGravity && Now % 1167 < 50);
  RPU_SetLampState(LAMP_PARTICLE_LEFT, !SparkAtAntiGravity && (Now + 1100) % 1167 < 50);
  // After a Spark Bonus is collected its value lamp blinks quickly for about 3.3 s, and from 1.5 s to 4.2 s
  // SPARK BONUS and ANTI GRAVITY flash. Otherwise SPARK BONUS and the value lamps are off, and ANTI GRAVITY
  // flashes only while the spark is there, with its value lamp blinking (bench, hardware's comparison).
  boolean sparkShow = SparkCollectedAt && Now - SparkCollectedAt >= 1500 && Now - SparkCollectedAt < 4200;
  boolean valueBlink = (Now % 333) < 50;
  boolean showValue = (SparkCollectedAt && Now - SparkCollectedAt < 3300) || SparkAtAntiGravity;
  RPU_SetLampState(LAMP_ANTI_GRAVITY, (SparkAtAntiGravity || sparkShow) && (Now / 300) % 2);
  // A.G. Entrance twice 150 ms apart every 1.12 s, A.G. Exit once every 1.17 s (bench: rmsg-c)
  RPU_SetLampState(LAMP_AG_ENTRANCE, SparkAtAntiGravity && (Now % 1117 < 50 || (Now % 1117 >= 150 && Now % 1117 < 200)));
  RPU_SetLampState(LAMP_AG_EXIT, Now - AntiGravityEnterTime < AG_WINDOW_MS && Now % 1167 < 50);
  RPU_SetLampState(LAMP_SPARK_BONUS, sparkShow && (Now / 333) % 2);
  for (byte i = 0; i < 3; i++)
    RPU_SetLampState(LampsSpark[i], InMultiball() ? (Now % 600 >= 200UL * i && Now % 600 < 200UL * i + 50) : (showValue && valueBlink && SparkLevel == i));
}

// Brain Transplant: the left star flashes once there's bonus
void ShowBrainLamps(boolean slow) {
  RPU_SetLampState(LAMP_BRAIN_LEFT, BonusK >= 1 && slow);
  RPU_SetLampState(LAMP_BRAIN_RIGHT, false);
}

// The Atomic Generator: the thumpers lit (not tilted), its flashers with a hit and a volts step
void ShowGeneratorLamps() {
  boolean on = !Tilted;
  RPU_SetLampState(LAMP_THUMPER_LEFT, on);
  RPU_SetLampState(LAMP_THUMPER_MIDDLE, on);
  RPU_SetLampState(LAMP_THUMPER_RIGHT, on);
  RPU_SetLampState(LAMP_ATOMIC_GEN, Now < AtomicFlashUntil);
  RPU_SetLampState(LAMP_VOLTS, Now < VoltsFlashUntil && Now % 250 < 50);
}

// The Atom Smasher: Load Atoms, the SMASH arrow, Matter Transmitter, the multiball flashers
void ShowSmasherLamps(boolean dark, boolean blink, boolean slow) {
  // Load Atoms while the smasher will trap a ball; the arrow (SMASH) while there's a trapped ball to hit out
  boolean trapping = !SmasherReleasing() && !Tilted;
  RPU_SetLampState(LAMP_LOAD_ATOMS_LEFT, trapping && slow);    // flashing (bench)
  RPU_SetLampState(LAMP_LOAD_ATOMS_RIGHT, trapping && !slow);
  RPU_SetLampState(LAMP_MATTER_TRANS, !SparkAtAntiGravity && !dark && blink);   // flashes with the Particle Separator (bench)
  RPU_SetLampState(LAMP_ATOM_SMASHER_ARROW, trapping && PinsUp && BallsLocked && slow);
  RPU_SetLampState(LAMP_ATOM_TOP, InMultiball() && Now % 250 < 50);
  RPU_SetLampState(LAMP_ATOM_BOTTOM, InMultiball() && (Now + 125) % 250 < 50);
}

void ShowPlayLamps() {
  boolean blink = (Now / 125) % 2, slow = (Now / 250) % 2;
  boolean dark = Now < AGDarkUntil;   // after an Anti-Gravity completion
  ShowWordLamps();
  ShowVoltsLamps();
  ShowBonusLamps();
  ShowPlayMultLamps();
  ShowLabLamps();
  ShowBubbleLamps(dark);
  ShowAwardLamps(blink, slow);
  ShowSparkLamps(dark, blink, slow);
  ShowBrainLamps(slow);
  ShowGeneratorLamps();
  ShowSmasherLamps(dark, blink, slow);
  AGShowLamps();   // over the ones above while it runs
}

// The original's attract light show (AttractShow.h: when each sweep starts, and the backbox chase), its sweeps
// played as the RPU's lamp animations (LampAnimations.h), from the game over sound or attract
unsigned long AttractShowFrom = 0;
void ShowAttractShowLamps();
void ShowAttractLamps() {
  // After power-on the show starts 7.17 s in, from its beginning (bench: poweron); the lamps are dark till then
  if (!AttractShowFrom) AttractShowFrom = Now < 7173 ? 7173 : Now;
  // All off, then this moment's lamps on, with RPU's lamp interrupt held off so no lamp it refreshes in between
  // is caught off (some tens of microseconds)
  noInterrupts();
  ShowAttractShowLamps();
  interrupts();
}
void ShowAttractShowLamps() {
  for (byte lamp = 0; lamp < 95 && lamp < RPU_MAX_LAMPS; lamp++) RPU_SetLampState(lamp, 0);
  if (Now < AttractShowFrom) return;
  long t = (Now - AttractShowFrom) % ATTRACT_SHOW_CYCLE;
  // The latest sweep started, and the two before it: a sweep's lamps stay lit past the next one's start
  const int beats = sizeof(AttractBeatAt) / sizeof(AttractBeatAt[0]);
  int latest = -1;
  for (int i = 0; i < beats; i++) if ((long)AttractBeatAt[i] <= t) latest = i;
  for (int back = 0; back < 3; back++) {
    int i = latest - back;
    long at;
    if (i >= 0) at = AttractBeatAt[i];
    else { i += beats; at = (long)AttractBeatAt[i] - ATTRACT_SHOW_CYCLE; }
    long local = t - at;
    if (local >= 0 && local < (long)LAMP_ANIMATION_STEPS * ANIMATION_STEP_MS)
      ShowLampAnimation(ANIMATION_ATTRACT_SWEEP_0 + AttractBeatSweep[i], ANIMATION_STEP_MS, local, 0, false);
  }
  // The backbox chase: the latest tick (or the cycle's last, before the first), a lamp 50 ms every 83.3 ms
  const int ticks = sizeof(AttractChaseAt) / sizeof(AttractChaseAt[0]);
  long tick = (long)AttractChaseAt[ticks - 1] - ATTRACT_SHOW_CYCLE;
  for (int i = 0; i < ticks; i++) if ((long)AttractChaseAt[i] <= t) tick = AttractChaseAt[i];
  long c = t - tick;
  if (c >= 0 && c < 1000) {
    byte step = c * 3 / 250;
    if (step < 12 && c - step * 250L / 3 < 50) RPU_SetLampState(AttractChaseLamp[step] - 1, 1);   // log numbers are ours + 1
  }
}

// ------------------------------------------------------------------ game flow
// Before each ball: TOP FLIPPER LOADS ATOMS with Anti-Gravity x6, 0.2 s apart; the serve 1.2 s later (more
// at the start of a game, after the STRANGE SCIENCE build-in)
unsigned long NextIntroDelay = 0, PinsDownAt = 0, StartPinsDownAt = 0;
byte DrainsThisGame = 0;
#define BALL_INTRO_MS         1150   // the first 72 to the serve sound (bench: 25 drains, 1.15-1.17 s)
#define INTRO_LEAD            83     // StartBall to the first 72, so the pins' coil can fire 33 ms before it

void StartGame() {
  if (EntryActive) return;   // not while a high score's initials go in (the phone's Start too)
  if (BallsNotFoundAt) return;
  if (!AllBallsHome()) { BallsNotFound(); return; }
  AttractShowFrom = 0;   // the next attract show starts with this game's 100
  if (!FreePlay) {
    if (!Credits) return;
    Credits--;
    RPU_WriteByteToEEProm(RPU_CREDITS_EEPROM_BYTE, Credits);
  }
  NumPlayers = 1;
  CurrentPlayer = 0;
  CurrentBall = 1;
  HighScoreToBeat = HighScore;
  for (byte p = 0; p < 4; p++) {
    Scores[p] = 0; Volts[p] = 0; ThumperHits[p] = 0; HeldBonus[p] = 0;
    PlayerStrange[p] = PlayerScience[p] = PlayerLab[p] = 0; PlayerSparkAtAG[p] = false;
    Replay1Given[p] = Replay2Given[p] = HighScoreGiven[p] = false;
  }
  ShootAgain = false;
  ServePending = false;
  ServeAt = 0;
  RPU_TurnOffAllLamps();
  RPU_EnableSolenoidStack();
  // The machine's balls: home, and the atoms still loaded from the last game
  TotalBalls = BallsHome() + BallsLocked;
  if (TotalBalls == 0 || TotalBalls > MAX_BALLS) TotalBalls = MAX_BALLS;
  ReleaseAllMultiball = false;
  UpdateBallCounts();
  LogBalls("game start");
  // The pins' down coil 95 ms after Start, with no atoms loaded, when they last moved 20 s or more before
  // (bench: p4's Start 34 s after power-on has it; five's and held2p's at 16 and 22 s don't; provisional)
  if (!BallsLocked && Now - LastPinsMove >= 20000) StartPinsDownAt = Now + 95;
  RPU_WriteULToEEProm(RPU_TOTAL_PLAYS_EEPROM_START_BYTE, RPU_ReadULFromEEProm(RPU_TOTAL_PLAYS_EEPROM_START_BYTE) + 1);
  // The start: stop all twice, five guitar notes 0.5 s apart, the voice a second after the last
  ClearQueue();
  StopSounds();
  Queue(10, SND_STOP);
  for (byte i = 0; i < 5; i++) Queue(120 + 500UL * i, SND_START_GUITAR_1 + i);
  Queue(3160, SND_START_VOICE);
  NextIntroDelay = 4100;
  DrainsThisGame = 0;
  BuildInStart = Now + 230;
  BuildInAt = BuildInSteps;
  PinNodeCtrl_GameStart(Credits, FreePlay);
  SetMachineState(MACHINE_STATE_INIT_GAMEPLAY);
}

void AddPlayer() {
  if (NumPlayers >= 4 || CurrentBall != 1) return;
  if (!FreePlay) {
    if (!Credits) return;
    Credits--;
    RPU_WriteByteToEEProm(RPU_CREDITS_EEPROM_BYTE, Credits);
  }
  Scores[NumPlayers] = 0;
  NumPlayers++;
  PlaySound(SND_PLAYER_ADDED);
  PinNodeCtrl_PlayerAdded(NumPlayers);
  Serial.printf("players %d\n", NumPlayers);
}

void StartBall() {
  // Kept from the player's last ball: the letters (still rotating), L-A-B, where the Spark Bonus is. The
  // bonus, Collect and Hold Bonus and the Anti-Gravity ladder start again (bench).
  StrangeLetters = PlayerStrange[CurrentPlayer];
  ScienceLetters = PlayerScience[CurrentPlayer];
  StrangeHeld = ScienceHeld = 0;
  StrangeLatest = ScienceLatest = 0;
  StrangeToggle = ScienceToggle = 0;
  WordsFlashFrom = 0;
  LetterSoundHoldUntil = 0; LetterSoundSlots = false; LetterSoundsCount = 0;
  StrangeLastHit = ScienceLastHit = 0xFF;
  LabLetters = PlayerLab[CurrentPlayer];
  BonusK = 0;
  BonusMultiplier = 1;
  CollectBonusLit = HoldBonusLit = SpecialLit = false;
  SparkAtAntiGravity = PlayerSparkAtAG[CurrentPlayer];   // the Particle Separator flashes first in a game
  SparkLevel = 0;
  AGCompletions = 0;
  ExtraBallUntil = SpecialUntil = 0;
  SaverLit = SaverMode == SAVER_EVERY_BALL || (SaverMode == SAVER_ONCE && CurrentBall == 1 && !ShootAgain);
  SaverGoesOutAt = 0;
  boolean sameBall = ShootAgain;
  ShootAgain = false;
  Tilted = false;
  TiltWarnings = 0;
  LastTiltTime = 0;
  BallConfirmed = false;
  DroneFrom = 0;
  BallStartTime = Now;
  AntiGravityEnterTime = LeftOutlaneTime = 0;
  AGShowAt = AGDarkUntil = 0;
  MarqueeAt = ExtraBallShowAt = BubbleShowAt = AdvanceShowAt = LaneCountAt = LabScoreAt = TimesShowFrom = 0;
  WordsAwardAt = WordsBuildInAt = 0;
  MBDrone = MBDrained = false;
  LoadServed = false;
  LabCompletions = 0;
  ReleaseAwardAt = 0;
  LoadServeAt = ServeKickAt = ReleaseShowFrom = ReleaseShowUntil = 0;
  MBLoopHeld = false;
  CurrentBackground = 0;   // the drone (16 up) starts again after each serve (bench: every serve)
  ServeSoundAt = 0;
  RPU_TurnOffAllLamps();
  RPU_EnableSolenoidStack();
  // Trapped balls stay trapped from ball to ball (and player to player): the smasher is the machine's. The
  // original closes it again before the serve.
  ReleaseAllMultiball = false;
  // None trapped: the pins down 33 ms before the first 72, not at Start (bench, hardware's comparison; the
  // coil follows the call by about 33 ms)
  NextIntroDelay += INTRO_LEAD;
  if (BallsLocked) RaisePins();
  else PinsDownAt = Now + NextIntroDelay - 66;
  // Before a player's last ball, before the intro: the next replay score (FIRST REPLAY AT 1500000, or
  // ANOTHER REPLAY AT 3500000 once past the first), flashing 0.8 s with 44 x5 0.18 s apart (bench: rmsg-b, d,
  // e, c2, all one player; none in rmsg-p3's four drains with three players, none into a ball 3). Nothing once
  // past both. Provisional: the trigger, and whose score with more players (this player's here).
  ReplayMsgFrom = ReplayMsgUntil = 0;
  if (CurrentBall == BallsPerGame && CurrentBall > 1 && !sameBall && Scores[CurrentPlayer] < Replay2) {
    unsigned long from = NextIntroDelay;   // where the first 72 would be; the pins' coil doesn't move
    for (byte i = 0; i < 5; i++) Queue(from + 183UL * i, SND_COUNT);
    ReplayMsgFrom = Now + from + 100;
    ReplayMsgUntil = Now + from + 904;
    ReplayMsgAnother = Scores[CurrentPlayer] >= Replay1;
    NextIntroDelay += 904;
  }
  for (byte i = 0; i < 6; i++) Queue(NextIntroDelay + 190UL * i, SND_ANTI_GRAVITY);
  IntroWordsFrom = Now + NextIntroDelay + 17;   // just after the first Anti-Gravity sound
  IntroWordsUntil = IntroWordsFrom + 1130;
  ServeAt = Now + NextIntroDelay + BALL_INTRO_MS;
  BubbleFlashFrom = ServeAt - 1180;
  BubbleIndex = 1;
  BubbleHeldFlash = false;
  BubbleHoldEnd = 0;
  NextBubble = ServeAt + 880;
  NextIntroDelay = 0;
  PinNodeCtrl_BallStart(CurrentPlayer + 1, CurrentBall);
}

// The serve after the intro: flippers on, the serve music for the atoms loaded
void ServeAfterIntro() {
  if (!ServeAt || Now < ServeAt) return;
  ServeAt = 0;
  MessageUntil = 0;
  RPU_SetDisableFlippers(Tilted);
  if (!Tilted) PlaySound(BallsLocked >= 3 ? SND_SERVE_3 : BallsLocked ? SND_SERVE_1 : SND_SERVE_0);
  ServeSoundAt = Now;
  ServeKickAt = Now + 50;
}

boolean EndedBySlam = false;
byte MatchNumber = 0;
byte MatchedPlayers = 0;   // a bit a player
// The match (bench: rmsg-e, f, g, p2; t from the game's end, 1.72 s after the last 90 or 1.98 s after the last 91,
// which is when the next ball's first 72 would have been less 0.17 s): from 0.89 s the players' scores slide
// left a place every 67 ms until only their last two digits are left, at the left of each display. From 1.72 s
// the 96s, and from 1.77 s the match number spins at the right of each player's display; it stops 0.1 s after
// the last 96. For each match in turn: 124 and a knock (and a credit), the player's number dark for 0.53 s from
// just before it (0.6 s). Then 100, and just after it every display shows its score (00 with no player) for 4.3 s before
// attract. No MATCH word, and no lamp show of ours yet (the original's lamps sweep at the 100).
#define MATCH_SPIN_AT 1717
#define MATCH_SLIDE_LEAD 828   // the scores start sliding 0.83 s before the first 96 (bench: rmsg-f)
const unsigned int MatchSpin[15] = {0, 50, 116, 183, 250, 349, 449, 584, 749, 934, 1166, 1449, 1800, 2233, 2767};
unsigned long GameOverSoundAt = 0;   // t of the 100; 0 after a slam (no match)
boolean MatchShown = true;           // this game's match spin (not with the Match setting off)

// The high scores: four, with initials, the first the high score to date (RPU_HIGHSCORE too, for the replays).
// The original's defaults (rev A, and the owner's machine at 22:37 on 11-10): 5,555,555 to date, then
// 5,555,555, 4,000,000 and 3,000,000. Kept in EEPROM from byte 100: a marker, then score (4 bytes) and
// initials (3) for each
#define HS_EEPROM_START   100
#define HS_EEPROM_MARK    0xA7
const unsigned long HSDefaults[4] = {5555555UL, 5555555UL, 4000000UL, 3000000UL};
unsigned long HSScore[4];
char HSInitials[4][4];
void SaveHighScores() {
  RPU_WriteByteToEEProm(HS_EEPROM_START, HS_EEPROM_MARK);
  for (byte i = 0; i < 4; i++) {
    RPU_WriteULToEEProm(HS_EEPROM_START + 1 + 7 * i, HSScore[i]);
    for (byte c = 0; c < 3; c++) RPU_WriteByteToEEProm(HS_EEPROM_START + 5 + 7 * i + c, HSInitials[i][c]);
  }
  RPU_WriteULToEEProm(RPU_HIGHSCORE_EEPROM_START_BYTE, HSScore[0]);
}
// Back to the defaults, initials blank (a setting, the owner: never by itself)
void ResetHighScores() {
  for (byte i = 0; i < 4; i++) { HSScore[i] = HSDefaults[i]; strcpy(HSInitials[i], "   "); }
  HighScore = HSScore[0];
  SaveHighScores();
}
// At power-up: the table. The first time (no table yet) the high score to date already stored stays first,
// the rest from the defaults
void LoadHighScores() {
  if (RPU_ReadByteFromEEProm(HS_EEPROM_START) != HS_EEPROM_MARK) {
    for (byte i = 0; i < 4; i++) { HSScore[i] = HSDefaults[i]; strcpy(HSInitials[i], "   "); }
    HSScore[0] = RPU_ReadULFromEEProm(RPU_HIGHSCORE_EEPROM_START_BYTE, HSDefaults[0]);
    for (byte i = 1; i < 4; i++) if (HSScore[i] > HSScore[0]) HSScore[i] = HSScore[0];
    SaveHighScores();
  }
  for (byte i = 0; i < 4; i++) {
    HSScore[i] = RPU_ReadULFromEEProm(HS_EEPROM_START + 1 + 7 * i, HSDefaults[i]);
    for (byte c = 0; c < 3; c++) {
      char ch = (char)RPU_ReadByteFromEEProm(HS_EEPROM_START + 5 + 7 * i + c);
      HSInitials[i][c] = (ch >= 'A' && ch <= 'Z') ? ch : ' ';
    }
    HSInitials[i][3] = 0;
  }
  HighScore = HSScore[0];
}

// A player whose score makes the table puts their initials in after the match, in place of the 100 (owner's
// machine, rev B, 11-10 22:37 log at 810 s, one entry): 97, PLAYER n PLEASE / ENTER INITIALS for 1.6 s, then
// PLAYER n and the three places on display 1's last three, the one being entered blinking 160 ms on and off,
// each starting at A. The right cabinet button steps on through the letters, the left one back (space before
// A), Start enters the letter. After the third: 255 0.11 s later, the 100 0.16 s, and game over goes on.
// Provisional: which scores make the table (here, beating the 4th), the order for more than one player (here,
// player order), no auto-repeat, and the RPU's own 60 s time-out. The owner (2026-11-12): numbers too, after
// Z (the original has letters only); a PinNode player's initials will go in without asking, once the PinNode
// library can say what they are (PinNodeInitials)
#define ENTRY_INTRO_MS    1646
#define ENTRY_TIMEOUT_MS  60000UL
byte EntryPlayers[4], EntryPlaces[4], EntryCount = 0, EntryIndex = 0, EntryLetter = 0, EntryLetterIndex = 0;
unsigned long EntryFrom = 0, EntryLastPress = 0;   // t (from the game over) the entry, or this player's, began
#define ENTRY_CHARS 37   // space, A-Z, 0-9
char EntryChar(byte i) { return i == 0 ? ' ' : i <= 26 ? 'A' + i - 1 : '0' + i - 27; }
// A PinNode player's initials: the library's PinNodeCtrl_PlayerInitials (PinNode 0.3.22; slot 1-4, three
// characters, false for none), from the node's check-in. Weak, so this still builds with a library from before
// it, and asks everyone then
boolean PinNodeCtrl_PlayerInitials(byte slot, char out[3]) __attribute__((weak));
boolean PinNodeInitials(byte player, char *initials) {
  if (!PinNodeCtrl_PlayerInitials || !PinNodeCtrl_PlayerInitials(player + 1, initials)) return false;
  for (byte c = 0; c < 3; c++) if (!((initials[c] >= 'A' && initials[c] <= 'Z') || (initials[c] >= '0' && initials[c] <= '9') || initials[c] == ' ')) initials[c] = ' ';
  Serial.printf("initials: player %d from PinNode '%c%c%c'\n", player + 1, initials[0], initials[1], initials[2]);
  return true;
}

// The entry, from GAME_OVER (t from the state's start)
boolean EntryStarted = false;
void EntryDone(unsigned long t) {
  EntryActive = false;
  SaveHighScores();
  Queue(109, SND_STOP);
  GameOverSoundAt = t + 162;
  Queue(162, SND_GAME_OVER);
}
// The next player to ask, from EntryIndex: players whose initials PinNode knows go in at once
void EntryNextPlayer(unsigned long t) {
  char initials[4];
  while (EntryIndex < EntryCount && PinNodeInitials(EntryPlayers[EntryIndex], initials)) {
    for (byte c = 0; c < 3; c++) HSInitials[EntryPlaces[EntryIndex]][c] = initials[c];
    EntryIndex++;
  }
  if (EntryIndex >= EntryCount) { EntryDone(t); return; }
  EntryLetterIndex = 0;
  EntryLetter = 1;   // A
  EntryFrom = t;
  EntryLastPress = t;
}
// The letter shown goes in; after the third, the next player
void EntryEnterLetter(unsigned long t) {
  HSInitials[EntryPlaces[EntryIndex]][EntryLetterIndex] = EntryChar(EntryLetter);
  Serial.printf("initials: player %d place %d '%c'\n", EntryPlayers[EntryIndex] + 1, EntryLetterIndex + 1, EntryChar(EntryLetter));
  EntryLastPress = t;
  if (++EntryLetterIndex < 3) { EntryLetter = 1; return; }
  EntryIndex++;
  EntryNextPlayer(t);
}
boolean ShowEntry(unsigned long t) {
  if (!EntryActive || t < EntryFrom) return false;
  if (!EntryStarted) {
    EntryStarted = true;
    EntryIndex = 0;
    EntryNextPlayer(t);
    if (!EntryActive) return false;
  }
  if (t - EntryLastPress > ENTRY_TIMEOUT_MS) {   // the RPU's own: what's shown goes in
    EntryEnterLetter(t);
    if (!EntryActive) return false;
  }
  char player = '1' + EntryPlayers[EntryIndex];
  SetText(0, "PLAYER ");
  if (t - EntryFrom < ENTRY_INTRO_MS) {
    char line[8] = "XPLEASE";
    line[0] = player;
    SetText(1, line);
    SetText(2, "ENTER I");
    SetText(3, "NITIALS");
    return true;
  }
  char line[8] = "       ";
  line[0] = player;
  for (byte c = 0; c < EntryLetterIndex; c++) line[4 + c] = HSInitials[EntryPlaces[EntryIndex]][c];
  if ((t - EntryFrom - ENTRY_INTRO_MS) % 320 < 160) line[4 + EntryLetterIndex] = EntryChar(EntryLetter);
  SetText(1, line);
  SetText(2, "");
  SetText(3, "");
  return true;
}
// The buttons while it runs: right on, left back, Start enters (none of them do anything else meanwhile)
boolean EntrySwitch(byte sw, unsigned long t) {
  if (!EntryActive || (sw != SW_CREDIT && sw != SW_BUTTON_LEFT && sw != SW_BUTTON_RIGHT)) return false;
  if (!EntryStarted || t < EntryFrom + ENTRY_INTRO_MS) return true;
  if (sw == SW_BUTTON_RIGHT) EntryLetter = (EntryLetter + 1) % ENTRY_CHARS;
  else if (sw == SW_BUTTON_LEFT) EntryLetter = (EntryLetter + ENTRY_CHARS - 1) % ENTRY_CHARS;
  else { EntryEnterLetter(t); return true; }
  EntryLastPress = t;
  return true;
}

// The end of a game. The atoms loaded stay for the next game. The high score to date is kept; its replays
// were given as it was passed. Then the match (not after a slam tilt).
void EndGame() {
  PinNodeCtrl_GameEnd(Scores, NumPlayers, EndedBySlam ? PINNODE_END_SLAM : PINNODE_END_NORMAL);
  RPU_SetDisableFlippers(true);
  RPU_DisableSolenoidStack();
  ReleaseAllMultiball = false;
  for (byte p = 0; p < 4; p++) LastScores[p] = Scores[p];
  LastNumPlayers = NumPlayers;
  // Each score that beats the table's last goes in, the highest first; its initials come after the match
  EntryCount = 0;
  EntryActive = false;
  boolean taken[4] = {false, false, false, false};
  for (byte n = 0; n < NumPlayers; n++) {
    int best = -1;
    for (byte p = 0; p < NumPlayers; p++) if (!taken[p] && (best < 0 || Scores[p] > Scores[best])) best = p;
    taken[best] = true;
    if (Scores[best] <= HSScore[3]) continue;
    byte place = 3;
    while (place > 0 && Scores[best] > HSScore[place - 1]) place--;
    for (byte i = 3; i > place; i--) { HSScore[i] = HSScore[i - 1]; strcpy(HSInitials[i], HSInitials[i - 1]); }
    for (byte e = 0; e < EntryCount; e++) if (EntryPlaces[e] >= place && EntryPlaces[e] < 3) EntryPlaces[e]++;
    HSScore[place] = Scores[best];
    strcpy(HSInitials[place], "   ");
    EntryPlayers[EntryCount] = best;
    EntryPlaces[EntryCount++] = place;
  }
  // in player order
  for (byte a = 0; a < EntryCount; a++)
    for (byte b = a + 1; b < EntryCount; b++)
      if (EntryPlayers[b] < EntryPlayers[a]) {
        byte tp = EntryPlayers[a]; EntryPlayers[a] = EntryPlayers[b]; EntryPlayers[b] = tp;
        tp = EntryPlaces[a]; EntryPlaces[a] = EntryPlaces[b]; EntryPlaces[b] = tp;
      }
  if (EntryCount) {
    HighScore = HSScore[0];
    SaveHighScores();
  }
  ClearQueue();
  MatchedPlayers = 0;
  GameOverSoundAt = 0;
  MatchShown = MatchOn && !EndedBySlam;
  if (!EndedBySlam && !MatchOn) {
    // No match (the Match setting off): the 100 where the spin would start (provisional: the original's)
    GameOverSoundAt = MATCH_SPIN_AT;
    if (EntryCount) {
      EntryFrom = GameOverSoundAt - 57;
      EntryIndex = 0;
      EntryActive = true;
      EntryStarted = false;
      Queue(EntryFrom, SND_ENTER_INITIALS);
      GameOverSoundAt = 0xFFFFFFFFUL;
    } else {
      Queue(GameOverSoundAt, SND_GAME_OVER);
    }
  } else if (!EndedBySlam) {
    MatchNumber = random(10) * 10;
    for (byte i = 0; i < 15; i++) Queue(MATCH_SPIN_AT + MatchSpin[i], SND_MATCH_SPIN);
    unsigned long at = MATCH_SPIN_AT + 2767 + 1183;
    for (byte p = 0; p < NumPlayers; p++) {
      if (Scores[p] % 100 != MatchNumber) continue;
      MatchedPlayers |= 1 << p;
      Queue(at, SND_REPLAY);
      if (Credits < MaxCredits) {   // at the limit: no credit, no knock
        Queue(at - 25, 0, SOL_KNOCKER);   // the coil follows the call by about 50 ms: 18-32 ms after the 124 (bench)
        Credits++;
      }
      at += 985;
    }
    RPU_WriteByteToEEProm(RPU_CREDITS_EEPROM_BYTE, Credits);
    GameOverSoundAt = MatchedPlayers ? at - 985 + 2887 : MATCH_SPIN_AT + 2767 + 3067;
    if (EntryCount) {
      // The initials in place of the 100: 97 57 ms before it would have come (owner's machine: 3.01 s after
      // the last 96, the 100 3.07 s after it with no entry)
      EntryFrom = GameOverSoundAt - 57;
      EntryIndex = 0;
      EntryActive = true;
      EntryStarted = false;
      Queue(EntryFrom, SND_ENTER_INITIALS);
      GameOverSoundAt = 0xFFFFFFFFUL;   // set when the entry ends
    } else {
      Queue(GameOverSoundAt, SND_GAME_OVER);
    }
  } else {
    EntryCount = 0;   // none after a slam
  }
  EndedBySlam = false;
  NumPlayers = 0;
  SetMachineState(MACHINE_STATE_MATCH_MODE);
}

// The ball's over: the same player again (shoot again), the next player or ball, or the game's end
int NextBall() {
  PinNodeCtrl_BallEnd(CurrentPlayer + 1, CurrentBall, Scores[CurrentPlayer]);
  DrainsThisGame++;
  if (ShootAgain) return MACHINE_STATE_INIT_NEW_BALL;   // the same player, the same ball
  CurrentPlayer++;
  if (CurrentPlayer >= NumPlayers) { CurrentPlayer = 0; CurrentBall++; }
  if (CurrentBall > BallsPerGame) { EndGame(); return MACHINE_STATE_MATCH_MODE; }
  return MACHINE_STATE_INIT_NEW_BALL;
}

// ------------------------------------------------------------------ switches in play
// After a smash: no multiball sound of its own; the first 72 run about 2.35 s later, then 80 (bench)
// The five-ball release has the 72 runs without the 80s, the first 2.55 s after the 69 (bench: five)
boolean MBLoopOn = false, MBNo80 = false;
void StartMultiballSounds() {
  MBLoopOn = true;
  MBNo80 = false;
  MBLoopHeld = false;
  NextMB72 = Now + 2350;
  NextMB80 = NextMB72 + 1600;
}

unsigned long SparkValue() {
  if (InMultiball()) return 25000UL * ((Now / 200) % 3 + 1);   // a moving value during multiball
  return 25000UL * (SparkLevel + 1);
}

void CollectSpark(const char *top, const char *bottom, boolean lampShow) {
  unsigned long value = SparkValue();
  AddScore(value);
  ShowMessage(top, bottom, 1500);
  if (lampShow) SparkCollectedAt = Now ? Now : 1;   // Anti-Gravity has its own (AGShowLamps)
}

// The extra ball: shoot again after this one
void AwardExtraBall(unsigned long soundDelay = 0) {
  ShootAgain = true;
  if (soundDelay) Queue(soundDelay, SND_EXTRA_BALL); else PlaySound(SND_EXTRA_BALL);
  ExtraBallShowAt = Now + soundDelay + 102;
  MessageUntil = 0;
}

void AwardSpecial() {
  SpecialLit = false;
  SpecialUntil = 0;
  ShowMessage("SPECIAL", 0, 2000, 0);
  AwardReplays(1);
}

// STRANGE / SCIENCE: the lit letters rotate together down each word (E back round to S): a place every
// 600 ms at 100V, 67 ms quicker a volts step (133 ms at 800V; bench). Each of a side's 3 targets tests a fixed
// place, one of the middle 3 letters: top the 3rd, middle the 4th, bottom the 5th. Whatever letter is there
// when it's hit lights. A hit holds the rotation: the next step is a full step after it, and a new letter's
// flash (about 0.47 s) holds it longer, so quick hits stop the letters moving (bench).
unsigned long RotateMs() { return 600UL - 200UL * Volts[CurrentPlayer] / 3; }   // Volts 0 = 100V
byte RotateDown(byte letters) { return ((letters << 1) | (letters >> 6)) & 0x7F; }

// One rotation step of a word (bench: wone, the original's lamps). A new letter is held where it lit; the
// moving letters go on a place, and one that comes up onto a held letter pushes it on, and from then on it
// moves with them. With nothing moving, the held letters start moving at this step and move from the next.
void StepWord(byte &letters, byte &held) {
  held &= letters;
  byte movers = letters & ~held;
  if (!movers) { held = 0; return; }
  byte moved = RotateDown(movers), pushed = moved & held;
  while (pushed) {
    held &= ~pushed;
    pushed = RotateDown(pushed);
    moved |= pushed;
    pushed &= held;
  }
  letters = moved | held;
  // A held letter with a moving one right behind it moves from now, and those held right in front of it: they
  // go on together at the next step, as a push would, and a hit on one plays 51 (bench: wkeep880/940 and
  // words2, 17 hits each with toggles kept lit, the 7th E1 hit 51 with C1 moving behind I and E1)
  byte behind = RotateDown(letters & ~held) & held;
  while (behind) {
    held &= ~behind;
    behind = RotateDown(behind) & held;
  }
}

// Both words: 111 (in place of the word's sound), the Power Saver lights, the Spark Bonus advances, the timed
// Special flashes in the right outlane for 33 s, and both words start again (bench). 70K in all with the
// letter's 10K (bench: q111a/b, 162000 to 232000), shown after the build-in (see ShownScore)
void HoldLetterSounds(unsigned long holdMs);
void WordsDone() {
  Queue(470, SND_STRANGE_SCIENCE);
  HoldLetterSounds(470 + 4050);
  AddScore(60000);
  WordsBuildInAt = Now + 470 + 84;
  WordsAwardAt = Now + 470 + 3960;
  WordsAwardValue = 70000 * PlayMultiplier();
  SaverLit = true;
  SaverGoesOutAt = 0;   // earned back, even while a used one waits to go out
  if (SparkLevel < 2) SparkLevel++;
  SpecialUntil = SpecialTimerMs == 0xFFFFFFFFUL ? 0xFFFFFFFFUL : Now + SpecialTimerMs;
  StrangeLetters = ScienceLetters = 0;
  StrangeHeld = ScienceHeld = StrangeLatest = ScienceLatest = 0;
  StrangeToggle = ScienceToggle = 0;
  WordsFlashFrom = Now;
}

// The letter that finishes a word: 10K in all, with 108 (STRANGE) or 109 (SCIENCE); the word stays lit (bench)
// After a word's sound the original holds the letter targets: none until 1.53 s after a 108/109 (4.05 s after
// a 111), then one every 483 ms while any wait, the highest switch first (STRANGE's bottom, middle and top
// targets, then SCIENCE's: 98 slots with a choice over rmsg-c/c2/h/p2h, qboth2, words2 and the q111s, a hit
// made after the slots start included), a 51 handing on to the next at once (rmsg-c/c2/h/p2h after a 111: 51
// and 106 17 ms apart), then at once again once a slot finds none waiting. A held hit
// counts whole at its slot: its sound, its letter and its points (q111b: a hit 24 ms before the slot, the
// letter lit and the 5K counting up from the slot as usual; qboth2 and rmsg-c: the letter lamp 51-56 ms after
// its slot). Hits while the words flash after a 111 wait too (rmsg-c/c2's four 111s: every hit from the
// flash on gets its slot, in hit order; qboth2: a flash hit's 106 at the 2nd slot). Provisional: a hit on a
// target already waiting merges with it (q111b: two hits on one target, one 106; or a hit in the flash's last
// 30 ms is lost)
void LetterSound(byte sound) {
  LastLetterSound = sound;
  PlaySound(sound);
}
// The sound a hit would play now: how long it holds the rotation, and how long its slot is (see LightLetter)
void LightLetter(byte &letters, byte index);
byte LetterHitSound(byte &letters, byte index, int8_t again = -1) {
  boolean strange = (&letters == &StrangeLetters);
  byte held = strange ? StrangeHeld : ScienceHeld, latest = strange ? StrangeLatest : ScienceLatest;
  if (__builtin_popcount(letters) == 6 && ((letters >> index) & 1)) {
    byte missing = __builtin_ctz(~letters & 0x7F);
    if (missing != 1) index = missing;
  }
  if (letters == 0x7F) return SND_WORD_DONE_HIT;
  byte bit = 1 << index;
  boolean last = again < 0 ? latest == bit : again;
  if ((letters & bit) && !((held & bit) && !last)) return SND_LETTER_LIT;
  return SND_LETTER;
}
void HoldLetterSounds(unsigned long holdMs) {
  LetterSoundHoldUntil = LetterSoundSlotAt = Now + holdMs;
  LetterSoundSlots = true;
}
void UpdateLetterSounds() {
  if (!LetterSoundSlots || Now < LetterSoundSlotAt) return;
  if (!LetterSoundsCount) { LetterSoundSlots = false; return; }
  byte pick = 0;   // the highest switch first: STRANGE's bottom, middle, top, then SCIENCE's
  for (byte i = 1; i < LetterSoundsCount; i++) if (LetterHitsWaiting[i] > LetterHitsWaiting[pick]) pick = i;
  byte hit = LetterHitsWaiting[pick];
  LetterSlotAgain = LetterHitsAgain[pick];
  LetterSlotSteps = LetterHitsSteps[pick];
  byte &letters = hit & 0x80 ? StrangeLetters : ScienceLetters;
  for (byte i = pick; i + 1 < LetterSoundsCount; i++) {
    LetterHitsWaiting[i] = LetterHitsWaiting[i + 1];
    LetterHitsAgain[i] = LetterHitsAgain[i + 1];
    LetterHitsSteps[i] = LetterHitsSteps[i + 1];
  }
  LetterSoundsCount--;
  LetterSoundSlotAt = Now + 483;
  LastLetterSound = 0;
  LetterFromSlot = true;   // last, as the hit may finish a word and hold the sounds again
  LightLetter(letters, hit & 0x7F);
  LetterFromSlot = false;
  if (LastLetterSound == SND_LETTER_LIT && LetterSoundSlotAt == Now + 483) LetterSoundSlotAt = Now + 17;
}

void WordComplete(byte &letters) {
  AddScore(10000);
  WordFlashFrom = Now;
  WordFlashWord = &letters;
  if (StrangeLetters == 0x7F && ScienceLetters == 0x7F) WordsDone();
  else {
    Queue(470, &letters == &StrangeLetters ? SND_WORD_STRANGE : SND_WORD);   // 467-481 ms after (bench: 7 words)
    HoldLetterSounds(470 + 1533);
  }
}

// A target: a new letter 5K and 1K bonus; one already lit 1K and no bonus (the bench; the manual says 1K bonus
// too)
// Any STRANGE or SCIENCE target restarts the one rotation timer: it fires 25 ms after the hit, or as a new
// letter's flash ends (0.5 s), and every RotateMs() after. SCIENCE steps each time it fires; STRANGE misses
// the first, so it looks held for a step (bench: wsci, wstr, wone, strange, strange2/3, rmsg-c: lit +60/+650,
// new +530/+1115 on the bench for SCIENCE/STRANGE, from a hit on either word). The new letter flashes
// until the timer fires, and is held there (see StepWord; wone).
boolean SweepSkipsStrange = false;
void RotationAfterHit(unsigned long flash) {
  NextSweep = Now + 25 + flash;
  SweepSkipsStrange = true;
}

// A toggled letter goes dark as its flash ends, unless another letter of its word moves at that step: then it
// stays lit and held (bench: 404 toggles with nothing else moving at that step went dark; words2's two with a
// letter moving stayed lit, and the hits after them played as if the letter was still the last one hit. STRANGE
// doesn't step as the flash ends, so a STRANGE toggle always goes dark). Provisional: a hit on the same word
// before the step, which counts the letter as dark.
void EndToggle(byte &letters, byte &held, byte &toggle, boolean moving) {
  if (toggle && !moving) { letters &= ~toggle; held &= ~toggle; }
  toggle = 0;
}

// The rotation timer fires: both words step (STRANGE not at the first firing after a hit)
void SweepWords() {
  NextSweep = Now + RotateMs();
  LetterFlashWord = 0;   // a new letter's flash ends as the timer fires
  EndToggle(StrangeLetters, StrangeHeld, StrangeToggle, !SweepSkipsStrange && (StrangeLetters & ~StrangeHeld));
  byte was = StrangeLetters;
  if (!SweepSkipsStrange) StepWord(StrangeLetters, StrangeHeld);
  if (StrangeLetters != was) StrangeSteps++;
  SweepSkipsStrange = false;
  EndToggle(ScienceLetters, ScienceHeld, ScienceToggle, ScienceLetters & ~ScienceHeld);
  was = ScienceLetters;
  StepWord(ScienceLetters, ScienceHeld);
  if (ScienceLetters != was) ScienceSteps++;
  StrangeLatest &= StrangeHeld;
  ScienceLatest &= ScienceHeld;
}

void LightLetter(byte &letters, byte index) {
  // Letter sounds held after a word's sound (the 111's flash too): the hit waits, and counts whole at its slot
  // (see LetterSound)
  byte &lastHit = (&letters == &StrangeLetters) ? StrangeLastHit : ScienceLastHit;
  boolean again = (lastHit == index);
  if (!LetterFromSlot) lastHit = index;
  if (!LetterFromSlot && (Now < LetterSoundHoldUntil || LetterSoundSlots)) {
    byte hit = (&letters == &StrangeLetters ? 0x80 : 0) | index;
    // A hit in the hold holds the rotation now, as any hit does (rmsg-c/c2: STRANGE still through the 109's
    // hold); one while the slots run doesn't (rmsg-c's first 111: a W18 made 180 ms after its slot lit E1, and
    // the step still came as the E1's flash ended, so the W18 played 51 at the next slot)
    if (Now < LetterSoundHoldUntil) RotationAfterHit(LetterHitSound(letters, index, again) == SND_LETTER_LIT ? 0 : 470);
    for (byte i = 0; i < LetterSoundsCount; i++) if (LetterHitsWaiting[i] == hit) return;
    if (LetterSoundsCount < sizeof(LetterHitsWaiting)) {
      LetterHitsAgain[LetterSoundsCount] = again;
      LetterHitsSteps[LetterSoundsCount] = (&letters == &StrangeLetters) ? StrangeSteps : ScienceSteps;
      LetterHitsWaiting[LetterSoundsCount++] = hit;
    }
    return;
  }
  // A step due within 17 ms happens first (bench: wstr's 4th hit, a STRANGE step just after the switch,
  // counted before the target; wkeep1000's 6th hit, a SCIENCE step about 26 ms after it, didn't in one run of
  // five. The original's own steps vary by up to 47 ms between runs, so these are coin-flips there too.
  // Provisional: the edge)
  if ((long)(NextSweep - Now) <= 17) SweepWords();
  boolean strange = (&letters == &StrangeLetters);
  byte &held = strange ? StrangeHeld : ScienceHeld, &latest = strange ? StrangeLatest : ScienceLatest;
  byte &toggle = strange ? StrangeToggle : ScienceToggle;
  EndToggle(letters, held, toggle, false);
  // Six lit and the target's own place lit: it lights the missing letter, which finishes the word, unless the
  // missing letter is the word's 2nd (bench: 27 finishes with the dark place 1st, 5th, 6th or 7th, over wsci,
  // words, words2, qboth2, rmsg-c/c2/h/p2h; with the 2nd dark, rmsg's hits toggled or played 51 instead.
  // Provisional: why the 2nd)
  if (__builtin_popcount(letters) == 6 && ((letters >> index) & 1)) {
    byte missing = __builtin_ctz(~letters & 0x7F);
    if (missing != 1) index = missing;
  }
  if (letters == 0x7F) {   // a completed word stays lit: 1K a hit (bench), and flashes whole a moment (rmsg-c)
    WordFlashFrom = Now;
    WordFlashWord = &letters;
    AddScore(1000);
    LetterSound(SND_WORD_DONE_HIT);
    RotationAfterHit(470);   // the flash holds the rotation as a new letter's does (rmsg-c, words2: the other word stops)
    return;
  }
  byte bit = 1 << index;
  if (letters & bit) {
    // A letter that hasn't started moving, other than the word's last new one: the hit toggles it, and it
    // scores and sounds as a new letter would, flashing, then dark or not (see EndToggle) (bench: words2,
    // rmsg-c/c2/h/p2h, qboth; 1024 hits replayed). Any other lit letter: 51 (brain: the last new letter hit
    // again)
    // A waiting hit on the word's last new letter plays 51 only if it repeated the word's last target hit as
    // it was made, or the word has stepped since it was made; otherwise it toggles (rmsg-c's 1st 111: A lit
    // at slot 2, its own waiting hit made after one on R toggled it at slot 3, STRANGE not stepping; E1 lit
    // at a slot, its waiting hit made after one on I played 51 at the next, SCIENCE stepping first there;
    // q111b's 2nd run: two R hits, the slot's R then 51. Provisional: three cases)
    boolean last = latest == bit;
    if (LetterFromSlot) last = last && (LetterSlotAgain || (strange ? StrangeSteps : ScienceSteps) != LetterSlotSteps);
    if ((held & bit) && !last) {
      toggle = bit;
      latest = bit;
      RotationAfterHit(470);
      LetterFlashFrom = Now;
      LetterFlashWord = &letters;
      LetterFlashIndex = index;
      AddBonus(1);   // provisional: the bonus, as for a new letter
      AddScore(5000);
      CountUp(5000 * PlayMultiplier(), 470);
      LetterSound(SND_LETTER);
      return;
    }
    AddScore(1000);
    LetterSound(SND_LETTER_LIT);
    RotationAfterHit(0);
    return;
  }
  letters |= bit;
  held |= bit;
  latest = bit;
  RotationAfterHit(470);
  LetterFlashFrom = Now;
  LetterFlashWord = &letters;
  LetterFlashIndex = index;
  AddBonus(1);
  if (letters == 0x7F) { WordComplete(letters); return; }
  AddScore(5000);
  CountUp(5000 * PlayMultiplier(), 470);
  LetterSound(SND_LETTER);
}

// Letters spotted by an Anti-Gravity completion or a load (one) or a Particle Separator hole (two), for no
// points: SCIENCE letters up to six; once six are lit, one STRANGE letter instead, up to six. A spot never
// lights a word's last letter, so it never finishes a word (bench: ladder3's lamps, SCIENCE held at six from
// the 3rd hole with its dark letter rotating, STRANGE one a hole or completion after it, both at six by the
// end). Provisional: which place lights.
void LightNextLetter(byte &letters) {
  byte next = 0;
  while ((letters >> next) & 1) next++;
  letters |= (1 << next);
}
void SpotLetters(byte count) {
  if (__builtin_popcount(ScienceLetters) < 6) {
    while (count-- && __builtin_popcount(ScienceLetters) < 6) LightNextLetter(ScienceLetters);
  } else if (__builtin_popcount(StrangeLetters) < 6) {
    LightNextLetter(StrangeLetters);
  }
}

// An L-A-B letter: 5K new (55), 1K lit (53); a hole's letter scores the same without its sound. The letter
// that finishes L-A-B scores only the 10K: 110, the letters reset and the next BONUS MULT lamp lights (the ones
// before stay lit), so the bonus counts 2X after one, 3X after two, up to 6X after five (bench).
void LabLetter(byte letter, boolean sound = true) {
  if (LabLetters & (1 << letter)) {
    AddScore(1000);
    if (sound) PlaySound(SND_LAB_LIT);
    return;
  }
  LabLetters |= (1 << letter);
  if (sound) AddBonus(1);   // a lane's new letter, the last one too, adds 1K bonus (hardware's comparison);
                            // the holes' bonus counts already include their letter (bench)
  if (LabLetters == 0x07) {
    LabScoreAt = Now + 3390;   // the 10K shows as the message ends (bench: hold2, qlab5)
    AdvanceShowAt = Now + 1930;
    PlaySound(SND_LAB_DONE);
    for (byte i = 0; i < 8; i++) Queue(1880 + 195UL * i, SND_MB_LOOP_1);   // 72 x8 after it (bench)
    if (LabCompletions < 5) LabCompletions++;
    BonusMultiplier = LabCompletions + 1;
    LabLetters = 0;
    return;
  }
  AddScore(5000);
  if (sound) {
    PlaySound(SND_LAB_LETTER);
    CountUp(5000 * PlayMultiplier(), 388);
  }
}

// An upper L-A-B hole: the Particle Separator. Its L-A-B letter and two SCIENCE letters; flashing, the Spark
// Bonus and 4K bonus, and Anti-Gravity flashes next; otherwise 10K and 2K bonus. PARTICLE SEPARATOR and the
// value scroll across the displays (bench). Not flashing, with its letter already lit: 53 0.07 s after (bench:
// 10 of 10 plain holes, never at a flashing one; the original's 53 comes 21-31 or 73-82 ms after the switch,
// which looks like a task's phase. Provisional: a plain hole with its letter not lit, which no script has).
void SeparatorHole(byte letter) {
  PlaySound(SND_EFFECT);
  // The hole holds both words' rotation: they step next 0.72 s after it, STRANGE too, then as usual (bench: 44
  // of 44 holes in the original's logs, 644-790 ms)
  NextSweep = Now + 720;
  SweepSkipsStrange = false;
  if (!SparkAtAntiGravity) {
    unsigned long value = SparkValue();
    CollectSpark("SPARK", "BONUS");
    StartMarqueeValue("PARTICLE SEPARATOR", value);
    SparkAtAntiGravity = true;
    AddBonus(4);
  } else {
    AddScore(10000);
    AddBonus(2);
    StartMarqueeValue("PARTICLE SEPARATOR", 10000);
    if (LabLetters & (1 << letter)) Queue(70, SND_LAB_LIT);
  }
  SpotLetters(2);
  LabLetter(letter, false);
}

// Anti-Gravity: the entrance, then the exit within 2.4 s: 10K and 1K bonus, and a SCIENCE letter. Flashing
// (after a Particle Separator hole), it collects the Spark Bonus instead, the Particle Separator flashes next,
// and it steps the ladder (bench, Award Count 3): Collect Bonus, Hold Bonus, the Extra Ball light, the Extra
// Ball, Special. Plain completions don't step it.
void AntiGravityDone() {
  SpotLetters(1);
  if (!SparkAtAntiGravity) {
    AddScore(10000);
    AddBonus(1);
    StartMarqueeValue("ANTI GRAVITY", 10000);
    Queue(58, SND_AG_EXIT);   // 50-67 ms after the exit's 152 (bench: antigrav, bubdark2)
    StartAGShow(false);
    return;
  }
  unsigned long value = SparkValue();
  CollectSpark("ANTI", "GRAVITY", false);
  if (AGCompletions + 1 != AG_EXTRA_BALL) StartMarqueeValue("ANTI GRAVITY", value);   // the Extra Ball has its own
  AddBonus(5);
  AGCompletions++;
  // Once the Extra Ball is lit the spark stays at Anti-Gravity until the Extra Ball is collected there (bench:
  // ladder3 and awards, the 4th hole is a plain one and the 4th Anti-Gravity collects the spark)
  SparkAtAntiGravity = (AGCompletions == AG_EXTRA_BALL_LIGHT);
  StartAGShow(true);
  // The ladder's sound 185 ms after the 152, and no 77 (bench, hardware's comparison)
  if (AGCompletions == 1) { CollectBonusLit = true; Queue(185, SND_LADDER_COLLECT); }
  else if (AGCompletions == 2) { HoldBonusLit = true; Queue(185, SND_LADDER_HOLD); }
  else if (AGCompletions == AG_EXTRA_BALL_LIGHT) Queue(185, SND_LADDER_EB_LIT);
  else if (AGCompletions == AG_EXTRA_BALL) AwardExtraBall(200);
  else if (AGCompletions == AG_SPECIAL_LIGHT) { SpecialLit = true; Queue(185, SND_LADDER_SPECIAL); }
  else Queue(185, SND_LADDER_COLLECT);
}

// --- the playfield's switches, each its own rules (from HandleGamePlaySwitches)

// The right outlane: 20K (shown 10K at a time at once: words2, +84, +217 ms), and a lit Special
void HandleRightOutlane() {
  AddScore(20000);
  if (!Tilted) CountUp(20000 * PlayMultiplier(), 0);   // shown 10K at a time at once (words2: +84, +217 ms)
  PlaySound(SND_RIGHT_OUTLANE);
  if (SpecialLit || Now < SpecialUntil) AwardSpecial();
}

// The left outlane: 3K; a lit Power Saver goes out 4 s later, used or not (the owner)
void HandleLeftOutlane() {
  AddScore(3000);
  LeftOutlaneTime = Now;
  PlaySound(SND_LEFT_OUTLANE);
  // A lit Power Saver goes out 4 s after the ball passes the left outlane, used or not (the owner)
  if (SaverLit && !Tilted && !SaverGoesOutAt) {
    SaverGoesOutAt = Now + SAVER_WINDOW_MS;
    Serial.println("power saver: left outlane, out in 4 s");
  }
}

// Anti-Gravity's entrance: 1K, and its 2.4 s for the exit
void HandleAntiGravityEntrance() {
  AddScore(1000);
  AntiGravityEnterTime = Now;
  PlaySound(SND_AG_ENTER);
}

// Anti-Gravity's exit: a completion within 2.4 s of the entrance, or an exit alone
void HandleAntiGravityExit() {
  // A completion; an exit alone is 1K, 1K a volts light and 1K bonus. A completion while the smash's timed
  // Extra Ball flashes collects it instead (123; bench).
  if (AntiGravityEnterTime && Now - AntiGravityEnterTime < AG_WINDOW_MS && Now < ExtraBallUntil) {
    AntiGravityEnterTime = 0;
    ExtraBallUntil = 0;
    AwardExtraBall();
    Queue(TIMED_END_MS, SND_TIMED_EB_OVER);
    return;
  }
  if (AntiGravityEnterTime && Now - AntiGravityEnterTime < AG_WINDOW_MS) {
    // The ladder's Extra Ball: 123 alone, 0.2 s after the exit (bench: ladder3 AG4)
    if (!(SparkAtAntiGravity && AGCompletions + 1 == AG_EXTRA_BALL)) PlaySound(SND_EFFECT);
    AntiGravityEnterTime = 0;
    AddScore(1000);   // the exit's 1K: a pass scores 12K in all (bench)
    AntiGravityDone();
  } else {
    PlaySound(SND_AG_EXIT);   // 77 alone, no 152 (bench: antigrav, an exit after a completion)
    AddScore(1000 + 1000UL * (Volts[CurrentPlayer] + 1));   // a volts light: 100V is the first
    AddBonus(1);
  }
}

// The left cabinet button: the Power Saver's flip
void HandlePowerSaverButton() {
  // The Power Saver flips from RPU's interrupt the moment the button closes, every press while it's lit
  // (see SetSaverArmed); it goes out 4 s after the left outlane (above). (If the left outlane switch
  // never reports a ball, the saver is never used up.)
  if (SaverLit && !Tilted) {
    PlaySound(SND_POWER_SAVER);
    Serial.println("power saver: flip");
  }
}

// The right cabinet button shifts the lit L-A-B letters
void ShiftLabLetters() {
  LabLetters = ((LabLetters << 1) | (LabLetters >> 2)) & 0x07;
}

// A bubble lane: the value lit now
void HandleBubbleLane(byte sw) {
  // The cycling value, frozen for a moment when it's made; three coin drops, higher for more. A step due
  // within 25 ms counts already (bench: the original pays a step its lamp shows up to 49 ms after the
  // switch, not one at 62-101, and the lamps show about 35 ms after the step: bubdark, bubdark2)
  if (Now >= BubbleHoldEnd && (long)(NextBubble - Now) <= 25) {
    NextBubble += 333;
    BubbleIndex = (BubbleIndex + 1) % 4;
  }
  AddScore(BubbleValues[BubbleIndex]);
  AddBonus(1);
  {
    // In a multiball 72 run its sounds and display wait until the run's end, as an 80 does (bench: bubx3)
    unsigned long wait = (InMultiball() && Now < MB72RunEnd) ? MB72RunEnd - Now : 0;
    BubbleShowAt = Now + wait;
    BubbleShowValue = BubbleValues[BubbleIndex] * PlayMultiplier();
    Queue(wait, SND_COIN_1 + BubbleIndex);   // three: 0.43 s, then 0.35 s apart (bench)
    Queue(wait + 433, SND_COIN_1 + BubbleIndex);
    Queue(wait + 783, SND_COIN_1 + BubbleIndex);
    if (wait && NextMB80 < Now + wait + 800) NextMB80 = Now + wait + 800;   // the 80 after them (bubx3)
  }
  // The left lane's hold flashes its lamp; a hit while held keeps the flash's beat (bench: bubble, bubdark2)
  if (Now >= BubbleHoldEnd || (sw == SW_LEFT_BUBBLE_LANE && !BubbleHeldFlash)) {
    BubbleHeldAt = Now;
    BubbleHeldFlash = (sw == SW_LEFT_BUBBLE_LANE);
  }
  NextBubble = BubbleHoldEnd = Now + BUBBLE_HOLD_MS;
}

// A thumper: 1K; every 13th hit a volts step (100V-900V), its drone the step's sound (bench)
void HandleThumper() {
  AddScore(1000);
  AtomicFlashUntil = Now + 50;
  if (++ThumperHits[CurrentPlayer] % 13 == 0 && Volts[CurrentPlayer] < 8) {   // 100V-900V
    Volts[CurrentPlayer]++;
    VoltsFlashUntil = Now + 1000;
    UpdateBackground();        // the new level's drone is the step's sound (bench)
  }
  PlaySound(SND_THUMPER);
}

// The saucer: the bonus, or 5K
void HandleSaucer() {
  // Lit: the bonus times the multiplier, counted in with 44 (x10), and the lamp goes out; the bonus stays.
  // Unlit: 5K and 1K bonus, 73 75 (bench)
  if (CollectBonusLit) {
    AddScore(1000UL * BonusK * BonusMultiplier);
    CollectBonusLit = false;
    PlaySound(SND_COLLECT_LIT);
    for (byte i = 0; i < 10; i++) Queue(150 + 100UL * i, SND_COUNT);
    Queue(1200, SND_SAUCER);
    ShowMessage("COLLECT", "BONUS", 1500);
  } else {
    AddScore(5000);
    AddBonus(1);
    PlaySound(SND_COLLECT);
    Queue(150, SND_SAUCER);
  }
  SaucerHoldUntil = Now + 1000;
}

// Brain Transplant: the left star flashes once there's bonus (2K, else 130); the right one flashes from 9K
// bonus and collects it without the bonus multiplier (else 130). What lights the right one is unknown (not
// the bonus alone): 130 for now
void HandleBrainLeft() {
  if (BonusK >= 1) { AddScore(2000); PlaySound(SND_BRAIN_LEFT); }
  else { AddScore(130); PlaySound(SND_BRAIN_RIGHT); }
}
void HandleBrainRight() {
  AddScore(130);
  PlaySound(SND_BRAIN_RIGHT);
}

// Each switch in play to its rules
void HandlePlayfieldSwitch(byte sw) {
  switch (sw) {
    case SW_RIGHT_OUTLANE: HandleRightOutlane(); break;
    case SW_LEFT_OUTLANE: HandleLeftOutlane(); break;
    case SW_ENTER_ANTI_GRAVITY: HandleAntiGravityEntrance(); break;
    case SW_EXIT_ANTI_GRAVITY: HandleAntiGravityExit(); break;
    case SW_BUTTON_LEFT: HandlePowerSaverButton(); break;
    case SW_BUTTON_RIGHT: ShiftLabLetters(); break;
    case SW_LEFT_BUBBLE_LANE: case SW_RIGHT_BUBBLE_LANE: HandleBubbleLane(sw); break;
    case SW_SCIENCE_TOP: LightLetter(ScienceLetters, 2); break;
    case SW_SCIENCE_MIDDLE: LightLetter(ScienceLetters, 3); break;
    case SW_SCIENCE_BOTTOM: LightLetter(ScienceLetters, 4); break;
    case SW_STRANGE_TOP: LightLetter(StrangeLetters, 2); break;
    case SW_STRANGE_MIDDLE: LightLetter(StrangeLetters, 3); break;
    case SW_STRANGE_BOTTOM: LightLetter(StrangeLetters, 4); break;
    case SW_RETURN_LANES: AddScore(5000); PlaySound(SND_RETURN_LANE); break;
    case SW_THUMPER_MIDDLE: case SW_THUMPER_LEFT: case SW_THUMPER_RIGHT: HandleThumper(); break;
    case SW_SLING_LEFT: AddScore(130); PlaySound(SND_SLING_LEFT); break;
    case SW_SLING_RIGHT: AddScore(130); PlaySound(SND_SLING_RIGHT); break;
    case SW_SAUCER: HandleSaucer(); break;
    case SW_LOWER_LAB_L: LabLetter(0); break;
    case SW_LOWER_LAB_A: LabLetter(1); break;
    case SW_LOWER_LAB_B: LabLetter(2); break;
    case SW_UPPER_LAB_L: SeparatorHole(0); break;
    case SW_UPPER_LAB_A: SeparatorHole(1); break;
    case SW_UPPER_LAB_B: SeparatorHole(2); break;
    case SW_BRAIN_LEFT: HandleBrainLeft(); break;
    case SW_BRAIN_RIGHT: HandleBrainRight(); break;
    case SW_REBOUND: AddScore(230); break;
  }
}

boolean IsPlayfieldSwitch(byte sw) {
  if (sw == SW_SELF_TEST_SWITCH) return false;
  if (sw >= SW_TROUGH_5 && sw <= SW_TROUGH_1) return false;
  switch (sw) {
    case SW_BUTTON_LEFT: case SW_CREDIT: case SW_BUTTON_RIGHT: case SW_OUTHOLE: case SW_COIN_RIGHT:
    case SW_COIN_LEFT: case SW_COIN_MIDDLE: case SW_SLAM: case SW_TILT:
      return false;   // the shooter lane's bubble rollover counts: a ball that drains after it is over (bench)
  }
  return true;
}

// The switches every state handles: the coin-door button, the tests and the settings, the smasher's lanes,
// coins, Start, a high score's initials and the slam. True when the switch is taken here
boolean HandleSystemSwitches(byte sw) {
  if (sw == SW_SELF_TEST_SWITCH) {
    if (MachineState == MACHINE_STATE_ATTRACT) EnterOperatorMenu();
    else if (MachineState == MACHINE_STATE_DIAGNOSTICS) ExitDiagnostics();
    return true;
  }
  if (MachineState == MACHINE_STATE_DIAGNOSTICS) { DiagnosticsSwitch(sw); return true; }
  // The smasher's balls are counted in every state, and after a tilt
  if (sw == SW_LOAD_BALL_LANE) BallLoaded();
  if (sw == SW_UNLOAD_BALL_LANE) BallUnloaded();
  if (sw == SW_COIN_LEFT || sw == SW_COIN_MIDDLE || sw == SW_COIN_RIGHT) {
    AddCoin(sw == SW_COIN_LEFT ? 0 : sw == SW_COIN_MIDDLE ? 1 : 2);
    return true;
  }
  if (MachineState == MACHINE_STATE_MATCH_MODE && EntrySwitch(sw, Now - MachineStateTime)) return true;
  if (sw == SW_CREDIT) {
    if (MachineState == MACHINE_STATE_ATTRACT || MachineState == MACHINE_STATE_MATCH_MODE) StartGame();
    else AddPlayer();
    return true;
  }
  if (sw == SW_SLAM && NumPlayers) {
    SlamTilt();
    return true;
  }
  return false;
}

// A slam: the game ends at once, no match
void SlamTilt() {
  PinNodeCtrl_Tilt(CurrentPlayer + 1, PINNODE_TILT_SLAM);
  PlaySound(SND_SLAM);
  EndedBySlam = true;
  EndGame();
  Queue(300, SND_STOP);   // after EndGame: it clears the queue
  CurrentBackground = 0;
}

// The tilt bob. It swings against the ring several times per nudge: one warning per second at most
void HandleTiltBob() {
  if (Tilted || (LastTiltTime && Now - LastTiltTime < TILT_SETTLE_MS)) return;
  LastTiltTime = Now;
  if (++TiltWarnings > MaxTiltWarnings) {
    Tilted = true;
    RPU_SetDisableFlippers(true);
    RPU_DisableSolenoidStack();
    StopSounds();
    ClearQueue();
    PlaySound(SND_TILT);
    PinNodeCtrl_Tilt(CurrentPlayer + 1, PINNODE_TILT_TILT);
    Serial.println("tilt");
  } else {
    PlaySound(SND_TILT_WARNING);
    ShowMessage("WATCH", "OUT", 2000);
    PinNodeCtrl_Tilt(CurrentPlayer + 1, PINNODE_TILT_WARNING);
  }
}

// A playfield switch: the ball is in play. The first one starts the neon strobing and the drone
void NotePlayfieldSwitch() {
  if (!BallConfirmed) {
    StrobeNeon(random(3000, 10000));   // the ball is in play: the neon strobes 3-10 s
    DroneFrom = Now + 800;             // and the drone 0.8 s later, after the bubble's sounds (bench)
  } else if (LoadServed) {
    DroneFrom = Now + 800;             // after a load's plunge too (bench: five, loads 1-4)
    CurrentBackground = 0;
  }
  LoadServed = false;
  BallConfirmed = true;
  LastPlayfieldSwitch = Now;
}

// The switches in play: the tilt, then each playfield switch's own rules (HandlePlayfieldSwitch). Tilted, the
// playfield is dead (the saucer still gives its ball back)
void HandleGamePlaySwitches(byte sw) {
  if (sw == SW_TILT) { HandleTiltBob(); return; }
  if (Tilted) {
    if (sw == SW_SAUCER) SaucerHoldUntil = Now + 1000;
    return;
  }
  if (IsPlayfieldSwitch(sw)) NotePlayfieldSwitch();
  HandlePlayfieldSwitch(sw);
}

void HandleSwitch(byte sw) {
  Serial.printf("switch %d\n", sw == SW_SELF_TEST_SWITCH ? 0 : sw + 1);
  if (HandleSystemSwitches(sw)) return;
  if (MachineState == MACHINE_STATE_NORMAL_GAMEPLAY) HandleGamePlaySwitches(sw);
}

// ------------------------------------------------------------------ USB commands
byte SoundTestCode = 0;
unsigned long SoundTestUntil = 0;

// "neon on|off", "backbox on|off": the continuous outputs PB5 and PB7 (low = on).
// Atom smasher tests: "pins up|down" fires a coil; "trap N" says N balls are trapped (after loading them by
// hand); "release" lets them out as the game does (not in a game); "drop N" and "gap N" set the release's
// ms down per ball and ms between balls; "pulse N" the pins' coil pulse in half-cycles; "balls" prints the
// counts.
void SmasherCommand(const char *what, const char *how) {
  int n = atoi(how);
  if (!strcmp(what, "pins")) {
    if (!strcmp(how, "up")) RaisePins();
    else if (!strcmp(how, "down")) LowerPins();
    else { Serial.println("pins up|down"); return; }
  } else if (!strcmp(what, "drop")) {
    if (n < 20 || n > 1000) { Serial.println("drop N: 20-1000 ms"); return; }
    ReleaseDownMs = n;
  } else if (!strcmp(what, "gap")) {
    if (n < 300 || n > 10000) { Serial.println("gap N: 300-10000 ms"); return; }
    ReleaseGapMs = n;
  } else if (!strcmp(what, "pulse")) {
    if (n < 2 || n > 12) { Serial.println("pulse N: 2-12 half-cycles"); return; }
    PinPulse = n;
  } else if (!strcmp(what, "trap")) {
    if (n < 0 || n > MAX_BALLS) { Serial.println("trap N: 0-5"); return; }
    BallsLocked = n;
  } else if (!strcmp(what, "release")) {
    if (NumPlayers) { Serial.println("release: not in a game"); return; }
    StartRelease(false);
  }
  Serial.printf("smasher: pins %s, pulse %d, drop %u ms, gap %u ms\n", PinsUp ? "up" : "down", PinPulse,
                ReleaseDownMs, ReleaseGapMs);
  LogBalls("now");
}

void Command(char *line) {
  char what[12] = "", how[8] = "";
  sscanf(line, "%11s %7s", what, how);
  boolean on = !strcmp(how, "on");
  if (!strcmp(what, "pins") || !strcmp(what, "drop") || !strcmp(what, "gap") || !strcmp(what, "trap") ||
      !strcmp(what, "release") || !strcmp(what, "balls") || !strcmp(what, "pulse")) {
    SmasherCommand(what, how);
    return;
  }
  if (!strcmp(what, "snd")) {   // a sound board command, shown on the right module for 10 s
    int code = atoi(how);
    if (code < 1 || code > 255) { Serial.println("snd N: 1-255"); return; }
    RPU_PlaySound6803(code);
    SoundTestCode = code;
    SoundTestUntil = Now + 10000;
    Serial.printf("sound %d\n", code);
    return;
  }
  if (!strcmp(what, "neon")) SetNeon(on);   // attract keeps it off again, a game on
  else if (!strcmp(what, "backbox")) SetBackbox(on);
  else {
    Serial.println("snd N, neon on|off, backbox on|off, pins up|down, pulse N, trap N, release, drop N, gap N, balls (docs/console.md)");
    return;
  }
  Serial.printf("%s %s (PB %02X)\n", what, on ? "on" : "off", RPU_ReadContinuousSolenoids());
}

// ------------------------------------------------------------------ PinNode: link, tests from the phone
// Tests run only in diagnostics (the owner's rule), entered from the phone (the coin-door button opens the
// operator menus instead). Flags as in pinnode.game.json: the tests are local
// (Bluetooth at the machine), the coil test asks for a confirm, diagnostics in and out are local.
const PinNodeAction PinNodeActionsTable[] = {
  {PINNODE_ACTION_TEST | PINNODE_ACTION_LOCAL},                           // sound-test
  {PINNODE_ACTION_TEST | PINNODE_ACTION_LOCAL},                           // lamp-test
  {PINNODE_ACTION_TEST | PINNODE_ACTION_LOCAL | PINNODE_ACTION_CONFIRM},  // solenoid-test
  {PINNODE_ACTION_TEST | PINNODE_ACTION_LOCAL},                           // switch-test
  {PINNODE_ACTION_TEST | PINNODE_ACTION_LOCAL},                           // display-test
  {0},                                                                    // stop-sounds
  {PINNODE_ACTION_LOCAL | PINNODE_ACTION_ENTER_DIAG},                     // enter-diagnostics
  {PINNODE_ACTION_LOCAL | PINNODE_ACTION_EXIT_DIAG},                      // exit-diagnostics
  {PINNODE_ACTION_LOCAL | PINNODE_ACTION_CONFIRM},                        // reset-high-scores
  {PINNODE_ACTION_LOCAL | PINNODE_ACTION_CONFIRM},                        // clear-audits
};

#define TEST_NONE 0xFF
boolean SwitchTestState[48];
byte RunningTest = TEST_NONE;
uint16_t RunningTestReqId = 0;
long RunningTestArg = 0, RunningTestValue = 0;
unsigned long LastTestStep = 0;
int8_t TestOutputOn = -1;   // the neon or backbox in the coil test: -1 not yet set

void ShowTestDisplays(const char *name) {
  SetText(0, "TEST");
  SetText(1, name);
  RPU_SetDisplay(2, RunningTestValue, true, 1);
  RPU_SetDisplayBlank(3, 0);
}

// The neon and the backbox light back to normal (both on) after the coil test switched one
void RestoreContinuousOutputs() {
  SetNeon(true);
  SetBackbox(true);
}
// Test mode and the settings: the backbox lit, the neon off (the owner: it came on and stayed on), except
// while the coil test runs it
void ServiceOutputs() {
  SetNeon(false);
  SetBackbox(true);
}

void StopTest() {
  if (RunningTest == ACT_SOUND_TEST) StopSounds();
  if (RunningTest == ACT_LAMP_TEST) RPU_TurnOffAllLamps();
  if (RunningTest == ACT_SOLENOID_TEST) ServiceOutputs();
  RunningTest = TEST_NONE;
  SetText(0, "TEST"); SetText(1, ""); SetText(2, ""); SetText(3, "");
}

void StartTest(byte test, long arg) {
  if (RunningTest != TEST_NONE && RunningTest != test) StopTest();
  if (RunningTest == ACT_SOLENOID_TEST) ServiceOutputs();   // another coil picked
  RunningTest = test;
  RunningTestArg = arg;
  LastTestStep = 0;
  TestOutputOn = -1;
  switch (test) {
    case ACT_SOUND_TEST:
      RunningTestValue = arg;
      PlaySound((byte)arg);
      ShowTestDisplays("SOUND");
      break;
    case ACT_LAMP_TEST:
      RunningTestValue = arg;
      RPU_TurnOffAllLamps();
      RPU_SetLampState(arg, 1);
      ShowTestDisplays("LAMP");
      break;
    case ACT_SOLENOID_TEST:   // pulsed every 2 s while the test runs (see RunTest)
      RunningTestValue = 0;
      ShowTestDisplays("COIL");
      break;
    case ACT_SWITCH_TEST:   // both edges of every switch go to the phone (see RunTest)
      RunningTestValue = 0;
      // Switches already closed (stuck, or a ball sitting on one) go to the phone at once, not counted
      for (byte sw = 0; sw < 48; sw++) {
        SwitchTestState[sw] = RPU_ReadSingleSwitchState(sw);
        if (SwitchTestState[sw]) PinNodeCtrl_SwitchEvent(sw, true);
      }
      ShowTestDisplays("SWITCH");
      break;
    case ACT_DISPLAY_TEST:
      RunningTestValue = arg;
      break;
  }
}

// What a running test does over time
void RunTest() {
  if (RunningTest == ACT_SWITCH_TEST) {
    // The node keeps each switch's state: it needs the opens as well as the closes
    for (byte sw = 0; sw < 48; sw++) {
      boolean closed = RPU_ReadSingleSwitchState(sw);
      if (closed == SwitchTestState[sw]) continue;
      SwitchTestState[sw] = closed;
      PinNodeCtrl_SwitchEvent(sw, closed);
      if (closed) {
        RunningTestValue++;
        char text[8];
        snprintf(text, sizeof(text), "SW %d", sw + 1);
        SetText(1, text);
        RPU_SetDisplay(2, RunningTestValue, true, 1);
      }
    }
  }
  boolean onOffTest = (RunningTest == ACT_SOLENOID_TEST && (RunningTestArg == TEST_NEON || RunningTestArg == TEST_BACKBOX));
  if (onOffTest) {
    // Plain on/off on the coils' 2 s cadence, 1 s on and 1 s off (not the lightning)
    if (!LastTestStep || Now - LastTestStep > 2000) {
      LastTestStep = Now;
      RunningTestValue++;
      ShowTestDisplays(RunningTestArg == TEST_NEON ? "NEON" : "BACKBOX");
    }
    int8_t on = (Now - LastTestStep < 1000);
    if (on != TestOutputOn) {
      TestOutputOn = on;
      if (RunningTestArg == TEST_NEON) SetNeon(on);
      else SetBackbox(on);
    }
  }
  if (RunningTest == ACT_SOLENOID_TEST && !onOffTest && (!LastTestStep || Now - LastTestStep > 2000)) {
    LastTestStep = Now;
    byte coil = (byte)RunningTestArg;
    byte halfCycles = (coil == SOL_OUT_KICKER) ? 8 : (coil == SOL_OUTHOLE || coil == SOL_SAUCER) ? 6 : 4;
    Pulse(coil, halfCycles, true);
    RunningTestValue++;
    ShowTestDisplays("COIL");
  }
  if (RunningTest == ACT_DISPLAY_TEST) {
    static const char *const letters[4][2] = {{"ABCDEFG", "HIJKLMN"}, {"OPQRSTU", "VWXYZ"},
                                              {"0123456", "789"}, {"8888888", "8888888"}};
    byte page = (Now / 2000) % 2;
    if (RunningTestArg == 0) { for (byte d = 0; d < 4; d++) SetText(d, letters[3][0]); }
    else if (RunningTestArg == 1) { SetText(0, letters[page][0]); SetText(1, letters[page][1]); SetText(2, letters[1 - page][0]); SetText(3, letters[1 - page][1]); }
    else { SetText(0, letters[2][0]); SetText(1, letters[2][1]); SetText(2, letters[2][0]); SetText(3, letters[2][1]); }
  }
}

// Switches from RPU's stack in diagnostics: the switch test reads the states itself (RunTest)
void DiagnosticsSwitch(byte sw) {
  (void)sw;
}

void EnterDiagnostics() {
  ServiceOutputs();   // attract had the backbox flashing
  RPU_SetDisableFlippers(true);
  RPU_DisableSolenoidStack();
  RPU_TurnOffAllLamps();
  StopSounds();
  RunningTest = TEST_NONE;
  SetText(0, "TEST"); SetText(1, ""); SetText(2, ""); SetText(3, "");
  SetMachineState(MACHINE_STATE_DIAGNOSTICS);
}

// ------------------------------------------------------------------ the operator menus (the RPU's OperatorMenus)
// The RPU's coin-door menus (OperatorMenus.cpp, with its 6803 changes marked), names on the left display: the
// coin-door button enters them and steps through the levels (RETURN TO GAME, SELF TEST, AUDITS, BASIC
// ADJUST, RESETS, GAME ADJUST); the right cabinet button steps on, the left back; Start changes the value
// shown (held, the resets and an audit's clear). The keypad as well (the owner's map, 11-12: switches
// 0-based): A on, B back, the number keys type a value and ENTER puts it in (or steps on), CLR clears it,
// GAME leaves. A tournament's lock holds the difficulty settings (LOCKED, and a change is put back)
boolean CPCSelectionsHaveBeenRead = true;   // OperatorMenus' names for the coins per credit (the settings)
byte CPCSelection[3];
#define KEY_ENTER 0
#define KEY_CLR   2
#define KEY_GAME  3
#define KEY_A     11
#define KEY_B     19
const byte KeyDigits[10] = {1, 10, 9, 8, 18, 17, 16, 26, 25, 24};   // the switch for 0 to 9

// The settings in each adjustment menu, by their place in Settings[]
const byte BasicAdjSettings[] = {0, 1, 2, 3, 4, 5, 6, 7, 13, 14, 15};   // free play ... coins per credit
const byte GameAdjSettings[] = {8, 9, 10, 11, 12};                      // slingshots ... Power Saver
#define NUM_BASIC_ADJ (sizeof(BasicAdjSettings))
#define NUM_GAME_ADJ  (sizeof(GameAdjSettings))
#define RESET_HIGH_SCORES 0
#define RESET_FACTORY     1
#define CLEAR_AUDITS      2
const char *const ResetNames[3] = {"RESET HI SCORE", "RESET  FACTORY", "CLEAR   AUDITS"};
const char *const TestNames[6] = {"   LAMP   TEST", "DISPLAY   TEST", "   COIL   TEST", " SWITCH   TEST", "  SOUND   TEST", "  EJECT  BALLS"};
const char *const TopNames[6] = {"RETURN TO GAME", "   SELF   TEST", "  AUDITS      ", "BASIC   ADJUST", "  RESETS      ", " GAME   ADJUST"};

byte MenuAdjustByte = 0;         // the setting shown, as OperatorMenus changes it
unsigned long MenuAdjustUL = 0;
unsigned long MenuAuditValue = 0;
byte MenuSetting = 0xFF;         // its place in Settings[] (0xFF: none)
byte MenuReset = 0xFF;           // the reset shown
byte MenuSoundCode = 0;
unsigned long MenuLastServe = 0, MenuDoneAt = 0;

// OperatorMenus' look-ups: the tests' numbers (from 1) to lamps and coils
byte LampConvertDisplayNumberToIndex(byte number) {
  return (number >= 1 && number <= 95) ? number - 1 : OPERATOR_MENU_VALUE_OUT_OF_RANGE;
}
unsigned short SolenoidConvertDisplayNumberToIndex(byte number) {
  return (number >= 1 && number <= 15) ? number - 1 : OPERATOR_MENU_VALUE_OUT_OF_RANGE;
}
byte SolenoidConvertDisplayNumberToTestStrength(byte number) {
  byte coil = number - 1;
  return (coil == SOL_OUT_KICKER) ? 8 : (coil == SOL_OUTHOLE || coil == SOL_SAUCER) ? 6 : 4;
}
// The sound test: Start plays the next code, 1-255
byte SoundTestNext(byte n) {
  MenuSoundCode = n + 1;
  StopSounds();
  PlaySound(MenuSoundCode);
  return MenuSoundCode < 255;
}

void SetupOperatorMenus() {
  Menus.SetNavigationButtons(SW_BUTTON_RIGHT, SW_BUTTON_LEFT, SW_CREDIT, SW_SELF_TEST_SWITCH);
  Menus.SetKeypad(KeyDigits, KEY_A, KEY_B, KEY_ENTER, KEY_CLR, KEY_GAME);
  Menus.SetLampsLookupCallback(LampConvertDisplayNumberToIndex);
  Menus.SetSolenoidIDLookupCallback(SolenoidConvertDisplayNumberToIndex);
  Menus.SetSolenoidStrengthLookupCallback(SolenoidConvertDisplayNumberToTestStrength);
  Menus.SetSoundCallbackFunction(SoundTestNext);
  Menus.SetMenuButtonDebounce(250);
}

void EnterOperatorMenu() {
  StopSounds();
  RPU_TurnOffAllLamps();
  ServiceOutputs();
  MenuSetting = MenuReset = 0xFF;
  Menus.EnterOperatorMenu();
  Serial.println("operator menu");
}

// The setting shown: OperatorMenus changes MenuAdjustByte / MenuAdjustUL and writes its EEPROM place
void ShowSettingControls(byte setting) {
  MenuSetting = setting;
  const GameSetting &g = Settings[setting];
  static byte values[8];
  if (g.size == 4) {
    MenuAdjustUL = SettingValue[setting];
    Menus.SetParameterControls(OPERATOR_MENU_ADJ_TYPE_SCORE, 0, values, 0, g.eeprom, NULL, &MenuAdjustUL);
    return;
  }
  MenuAdjustByte = SettingValue[setting];
  if (g.eeprom >= RPU_CPC_CHUTE_1_SELECTION_BYTE && g.eeprom <= RPU_CPC_CHUTE_3_SELECTION_BYTE) {
    values[0] = 0; values[1] = NUM_CPC_PAIRS - 1;
    Menus.SetParameterControls(OPERATOR_MENU_ADJ_TYPE_CPC, 2, values, 0, g.eeprom, &MenuAdjustByte, NULL);
  } else if (g.labels) {
    byte n = g.maxValue - g.minValue + 1;
    for (byte k = 0; k < n && k < 8; k++) values[k] = g.minValue + k;
    Menus.SetParameterControls(OPERATOR_MENU_ADJ_TYPE_LIST, n, values, 0, g.eeprom, &MenuAdjustByte, NULL);
  } else {
    values[0] = g.minValue; values[1] = g.maxValue;
    Menus.SetParameterControls(OPERATOR_MENU_ADJ_TYPE_MIN_MAX, 2, values, 0, g.eeprom, &MenuAdjustByte, NULL);
  }
}

// A change from OperatorMenus: into the game's settings, unless a tournament holds it (then put back)
void TakeSettingChange() {
  if (MenuSetting == 0xFF) return;
  const GameSetting &g = Settings[MenuSetting];
  long v = g.size == 4 ? (long)MenuAdjustUL : (long)MenuAdjustByte;
  if ((g.difficulty && PinNodeCtrl_SettingLocked(MenuSetting)) || v < g.minValue || v > g.maxValue || (v - g.minValue) % g.step) {
    v = SettingValue[MenuSetting];
    if (g.size == 4) MenuAdjustUL = v; else MenuAdjustByte = v;
  }
  SettingValue[MenuSetting] = v;
  WriteParameter(MenuSetting);
  ApplyParameters();
  Serial.printf("settings: %d = %ld\n", MenuSetting + 1, v);
}

void DoReset(byte reset) {
  if (reset == RESET_HIGH_SCORES) ResetHighScores();
  if (reset == RESET_FACTORY) {
    for (byte i = 0; i < NUM_SETTINGS; i++) {
      if (Settings[i].difficulty && PinNodeCtrl_SettingLocked(i)) continue;
      SettingValue[i] = Settings[i].defaultValue;
      WriteParameter(i);
    }
    ApplyParameters();
  }
  if (reset == CLEAR_AUDITS) ClearAudits();
  MenuDoneAt = Now;
  Serial.printf("operator menu: %s\n", ResetNames[reset]);
}

// The level or item's name on the left module, a labelled value on the right (OperatorMenus shows numbers)
void ShowMenuNames() {
  byte top = Menus.GetTopLevel(), sub = Menus.GetSubLevel();
  const char *name = TopNames[top <= OPERATOR_MENU_GAME_ADJ_MENU ? top : 0];
  if (sub != OPERATOR_MENU_NOT_ACTIVE) {
    if (top == OPERATOR_MENU_SELF_TEST_MENU) {
      if (sub == OPERATOR_MENU_TEST_DISPLAYS || sub == OPERATOR_MENU_TEST_SWITCHES) return;   // the test owns them
      name = TestNames[sub];
      if (sub == OPERATOR_MENU_TEST_SOUNDS) RPU_SetDisplay(3, MenuSoundCode, true, 1);
    } else if (top == OPERATOR_MENU_AUDITS_MENU) name = Audits[sub].name;
    else if (top == OPERATOR_MENU_GAME_RULES_LEVEL) {
      name = ResetNames[sub];
      SetText(2, "");
      SetText(3, MenuDoneAt && Now - MenuDoneAt < 1500 ? "   DONE" : "  HOLD");
    } else if (MenuSetting != 0xFF) {
      name = Settings[MenuSetting].name;
      const GameSetting &g = Settings[MenuSetting];
      if (g.difficulty && PinNodeCtrl_SettingLocked(MenuSetting)) SetText(2, " LOCKED");
      if (g.labels && Menus.GetTypedValue() < 0) {
        SetText(2, "");
        SetText(3, g.labels[MenuAdjustByte - g.minValue]);
      }
    }
  }
  char left[8], right[8];
  memcpy(left, name, 7); left[7] = 0;
  memcpy(right, name + 7, 7); right[7] = 0;
  SetText(0, left);
  SetText(1, right);
}

void RunOperatorMenu() {
  boolean menuDone = !Menus.UpdateMenu(Now);
  // From the phone, "exit diagnostics" closes the menus too; nothing else runs here
  PinNodeActionRequest request;
  if (!menuDone && PinNodeCtrl_TakeAction(&request)) {
    if (request.op == PINNODE_OP_START && request.action == ACT_EXIT_DIAGNOSTICS) {
      Menus.ExitOperatorMenu();
      menuDone = true;
      PinNodeCtrl_ActionResult(request.reqId, request.action, PINNODE_ACTION_DONE, 0, PINNODE_REASON_FINISHED);
    } else {
      PinNodeCtrl_ActionResult(request.reqId, request.action, request.op == PINNODE_OP_STOP ? PINNODE_ACTION_DONE : PINNODE_ACTION_NOT_NOW, 0, request.reason);
    }
  }
  if (menuDone) {
    PinNodeCtrl_SettingsChanged();   // the node diffs
    PinNodeCtrl_AuditsChanged();     // they may have been cleared
    StopSounds();
    RPU_TurnOffAllLamps();
    SetMachineState(MACHINE_STATE_ATTRACT);
    Serial.println("operator menu: done");
    return;
  }
  // Eject balls: the trough's, one at a time to the shooter lane
  if (Menus.BallEjectInProgress() && TroughCount() && Now - MenuLastServe > 1500) {
    MenuLastServe = Now;
    Pulse(SOL_OUT_KICKER, 8, true);
  }
  byte top = Menus.GetTopLevel(), sub = Menus.GetSubLevel();
  if (Menus.HasTopLevelChanged()) {
    MenuSetting = MenuReset = 0xFF;
    if (top == OPERATOR_MENU_BASIC_ADJ_MENU) Menus.SetNumSubLevels(NUM_BASIC_ADJ);
    if (top == OPERATOR_MENU_GAME_RULES_LEVEL) Menus.SetNumSubLevels(3);
    if (top == OPERATOR_MENU_GAME_ADJ_MENU) Menus.SetNumSubLevels(NUM_GAME_ADJ);
  }
  if (Menus.HasSubLevelChanged() && sub != OPERATOR_MENU_NOT_ACTIVE) {
    MenuSetting = MenuReset = 0xFF;
    MenuDoneAt = 0;
    if (top == OPERATOR_MENU_AUDITS_MENU && sub < NUM_AUDITS) {
      MenuAuditValue = RPU_ReadULFromEEProm(Audits[sub].eeprom, 0);
      Menus.SetAuditControls(&MenuAuditValue, Audits[sub].eeprom, OPERATOR_MENU_AUD_CLEARABLE);
    } else if (top == OPERATOR_MENU_BASIC_ADJ_MENU && sub < NUM_BASIC_ADJ) {
      ShowSettingControls(BasicAdjSettings[sub]);
    } else if (top == OPERATOR_MENU_GAME_ADJ_MENU && sub < NUM_GAME_ADJ) {
      ShowSettingControls(GameAdjSettings[sub]);
    } else if (top == OPERATOR_MENU_GAME_RULES_LEVEL && sub < 3) {
      // A reset: held Start (OperatorMenus' two-value list, its held enter puts the second)
      static byte values[2] = {0, 1};
      static byte yes = 0;
      yes = 0;
      MenuReset = sub;
      Menus.SetParameterControls(OPERATOR_MENU_ADJ_TYPE_LIST, 2, values, 0, 0, &yes, NULL);
    }
  }
  if (Menus.HasParameterChanged()) {
    if (MenuReset != 0xFF) DoReset(MenuReset);
    else TakeSettingChange();
  }
  ShowMenuNames();
}

void ExitDiagnostics() {
  if (RunningTest != TEST_NONE) {
    PinNodeCtrl_ActionResult(RunningTestReqId, RunningTest, PINNODE_ACTION_DONE, RunningTestValue, PINNODE_REASON_DIAGNOSTICS);
    StopTest();
  }
  RPU_TurnOffAllLamps();
  SetMachineState(MACHINE_STATE_ATTRACT);
}

// A request from the phone: tests only in diagnostics, one-shots anywhere outside a game
void HandlePinNodeAction() {
  PinNodeActionRequest request;
  if (!PinNodeCtrl_TakeAction(&request)) return;
  boolean inDiagnostics = (MachineState == MACHINE_STATE_DIAGNOSTICS);
  boolean inGame = (NumPlayers != 0);

  if (request.op == PINNODE_OP_STOP) {
    long value = 0;
    if (RunningTest == request.action) { value = RunningTestValue; StopTest(); }
    PinNodeCtrl_ActionResult(request.reqId, request.action, PINNODE_ACTION_DONE, value, request.reason);
    return;
  }
  if (request.action <= ACT_DISPLAY_TEST) {
    if (!inDiagnostics) { PinNodeCtrl_ActionResult(request.reqId, request.action, PINNODE_ACTION_NOT_NOW, 0); return; }
    RunningTestReqId = request.reqId;
    StartTest(request.action, request.arg);
    PinNodeCtrl_ActionResult(request.reqId, request.action, PINNODE_ACTION_OK, RunningTestValue);
    return;
  }
  if (inGame) { PinNodeCtrl_ActionResult(request.reqId, request.action, PINNODE_ACTION_NOT_NOW, 0); return; }
  switch (request.action) {
    case ACT_STOP_SOUNDS: StopSounds(); break;
    case ACT_ENTER_DIAGNOSTICS:
      if (MachineState != MACHINE_STATE_ATTRACT && !inDiagnostics) {
        PinNodeCtrl_ActionResult(request.reqId, request.action, PINNODE_ACTION_NOT_NOW, 0);
        return;
      }
      if (!inDiagnostics) EnterDiagnostics();
      break;
    case ACT_EXIT_DIAGNOSTICS: if (inDiagnostics) ExitDiagnostics(); break;
    case ACT_RESET_HIGH_SCORES: ResetHighScores(); Serial.println("app: high scores reset"); break;
    case ACT_CLEAR_AUDITS: ClearAudits(); Serial.println("app: audits cleared"); break;
    default: PinNodeCtrl_ActionResult(request.reqId, request.action, PINNODE_ACTION_UNKNOWN, 0); return;
  }
  PinNodeCtrl_ActionResult(request.reqId, request.action, PINNODE_ACTION_DONE, 0, PINNODE_REASON_FINISHED);
}

// Start from the app: the credit button's path. In attract or after a game it starts one; during ball 1 it
// adds a player (up to 4); otherwise not now.
void HandleAppStart() {
  uint16_t reqId;
  if (!PinNodeCtrl_TakeStartRequest(&reqId)) return;
  byte playersBefore = NumPlayers;
  if (MachineState == MACHINE_STATE_ATTRACT || MachineState == MACHINE_STATE_MATCH_MODE) {
    StartGame();
    if (NumPlayers) PinNodeCtrl_StartResult(reqId, PINNODE_START_OK, 1);
    else PinNodeCtrl_StartResult(reqId, PINNODE_START_NO_CREDITS, 0);
  } else if (NumPlayers && CurrentBall == 1 && MachineState != MACHINE_STATE_DIAGNOSTICS) {
    if (NumPlayers >= 4) { PinNodeCtrl_StartResult(reqId, PINNODE_START_MAX_PLAYERS, 0); return; }
    AddPlayer();
    if (NumPlayers > playersBefore) PinNodeCtrl_StartResult(reqId, PINNODE_START_OK, NumPlayers);
    else PinNodeCtrl_StartResult(reqId, PINNODE_START_NO_CREDITS, 0);
  } else {
    PinNodeCtrl_StartResult(reqId, PINNODE_START_NOT_NOW, 0);
  }
}

// The manifest hash in PinNodeGame.h is 16 hex characters; HELLO carries it as 8 bytes
void StartPinNodeLink() {
  static byte hash[8];
  const char *hex = PINNODE_MANIFEST_HASH;
  for (byte count = 0; count < 16; count++) {
    char c = hex[count];
    byte nibble = (c >= 'a') ? (c - 'a' + 10) : (c >= 'A') ? (c - 'A' + 10) : (c - '0');
    if (count % 2 == 0) hash[count / 2] = nibble << 4;
    else hash[count / 2] |= nibble;
  }
  // The settings in the app: the same table as the menu, its order the manifest's settings[] index
  static PinNodeSetting table[NUM_SETTINGS];
  for (byte i = 0; i < NUM_SETTINGS; i++)
    table[i] = {Settings[i].eeprom, Settings[i].size, Settings[i].minValue, Settings[i].maxValue, 1, false,
                Settings[i].difficulty, Settings[i].step};
  PinNodeCtrl_Settings(table, NUM_SETTINGS, ReadStoredParameters);
  static PinNodeAudit audits[NUM_AUDITS];
  for (byte i = 0; i < NUM_AUDITS; i++) audits[i] = {Audits[i].eeprom, 4};
  PinNodeCtrl_Audits(audits, NUM_AUDITS);
  PinNodeCtrl_Actions(PinNodeActionsTable, sizeof(PinNodeActionsTable) / sizeof(PinNodeActionsTable[0]));
  RPULink_BeginUart();   // on the 400 this starts the SPI link to the XIAO
  PinNodeCtrl_Begin(PINNODE_RULESET_VERSION, PINNODE_RULESET_ID, hash, 4, 0, RPU_OS_HARDWARE_REV);
}

void UpdatePinNode() {
  RPULink_Update(Now);
  boolean inGame = (NumPlayers != 0);
  byte mode = (MachineState == MACHINE_STATE_DIAGNOSTICS || Menus.OperatorMenusActive()) ? PINNODE_MODE_SERVICE : inGame ? PINNODE_MODE_GAME : PINNODE_MODE_ATTRACT;
  PinNodeCtrl_Mode(mode, inGame ? CurrentBall : 0, inGame ? CurrentPlayer + 1 : 0);
  PinNodeCtrl_Scores(Scores, NumPlayers);
  PinNodeCtrl_Credits(Credits, FreePlay);
  if (!Menus.OperatorMenusActive()) HandlePinNodeAction();   // RunOperatorMenu takes them while it's open
  HandleAppStart();
  PinNodeCtrl_Update(Now, false);
}

// ------------------------------------------------------------------ setup and loop
// --- power-up and Start checks, as the original's (bench, 11-12: PinMAME, the original program)

// The original's names for its switches (by our switch number), as it shows a stuck one: the first word on
// display 2 (right-aligned), the rest on display 3. Not checked: the outhole, the trough and the matrix's unused
// places (null)
const char *const SwitchNames[48][2] = {
  {"RIGHT", "OUTLANE"}, {"LEFT", "OUTLANE"}, {"EXIT", "ANTI GR"}, {"ENTER", "ANTI GR"}, {"LEFT", "BUTTON"},
  {"CREDIT", "BUTTON"}, {"RIGHT", "BUTTON"}, {0, 0}, {"RIGHT", "COINS"}, {"LEFT", "COINS"}, {"MIDDLE", "COINS"},
  {"LEFT", "BUBBLE"}, {"RIGHT", "BUBBLE"}, {"SLAM", ""}, {"TILT", ""}, {"NOT", "USED"}, {"SCIENCE", "TGT TOP"},
  {"SCIENCE", "TGT MDL"}, {"SCIENCE", "TGT BTM"}, {"STRANGE", "TGT TOP"}, {"STRANGE", "TGT MDL"},
  {"STRANGE", "TGT BTM"}, {0, 0}, {0, 0}, {"RETURN", "LANES"}, {"MIDDLE", "BUMPER"}, {"LEFT", "BUMPER"},
  {"RIGHT", "BUMPER"}, {"LEFT", "SLING"}, {"RIGHT", "SLING"}, {0, 0}, {"RIGHT", "SAUCER"}, {0, 0}, {0, 0}, {0, 0},
  {0, 0}, {0, 0}, {"LOWER", "LAB L"}, {"LOWER", "LAB A"}, {"LOWER", "LAB B"}, {"BRAIN", "LEFT"}, {"BRAIN", "RIGHT"},
  {"UNLOAD", "BALL"}, {"LOAD", "BALL"}, {"BOUNCER", ""}, {"UPPER", "LAB L"}, {"UPPER", "LAB A"}, {"UPPER", "LAB B"}};

// A switch closed at power-up: the original says so from 0.17 s after its stop-all, a switch a second with 43,
// its name lit from 50 ms after the sound to 950 ms, the highest-numbered first, up to four switches, four
// rounds (and six showings for a lone switch), then attract (bench: one switch 6 showings, three 12, four of many
// 16). Provisional: which four when more are closed, and the count's rule
byte StuckSwitches[4], StuckCount = 0, StuckShowings = 0;
unsigned long StuckFrom = 0;   // the first showing's sound (0: none)
void FindStuckSwitches() {
  StuckCount = 0;
  for (int sw = 47; sw >= 0 && StuckCount < 4; sw--)
    if (SwitchNames[sw][0] && RPU_ReadSingleSwitchState(sw)) StuckSwitches[StuckCount++] = sw;
  if (!StuckCount) return;
  StuckShowings = StuckCount * 4 < 6 ? 6 : StuckCount * 4;
  StuckFrom = Now;
  for (byte i = 0; i < StuckShowings; i++) Queue(1003UL * i, SND_STUCK_SWITCH);
  Serial.printf("stuck switches: %d\n", StuckCount);
}
// True while it runs (it owns the displays)
boolean ShowStuckSwitches() {
  if (!StuckFrom) return false;
  unsigned long t = Now - StuckFrom;
  byte showing = t / 1003;
  if (showing >= StuckShowings) { StuckFrom = 0; return false; }
  unsigned long in = t % 1003;
  byte sw = StuckSwitches[showing % StuckCount];
  boolean lit = in >= 50 && in < 950;
  char word[8];
  snprintf(word, sizeof(word), "%7s", lit ? SwitchNames[sw][0] : "");
  SetText(0, "");
  SetText(1, word);
  SetText(2, lit ? SwitchNames[sw][1] : "");
  SetText(3, "");
  return true;
}

// Start without all five balls home (the trough's and the smasher's): no game. ALL BALLS NOT FOUND flashes 1.06 s
// (67 ms lit in 200), then lit 0.3 s, with 44 eight times 0.19 s apart, and attract starts over (bench: four in
// the trough; no coil fires, no credit taken). Provisional: the flash's exact beats
unsigned long BallsNotFoundAt = 0;
boolean AllBallsHome() { return TroughCount() + BallsLocked >= MAX_BALLS; }
void BallsNotFound() {
  BallsNotFoundAt = Now;
  for (byte i = 0; i < 8; i++) Queue(42 + 1535UL * i / 8, SND_BALLS_NOT_FOUND);
  Serial.printf("balls not found: trough %d, locked %d\n", TroughCount(), BallsLocked);
}
boolean ShowBallsNotFound() {
  if (!BallsNotFoundAt) return false;
  unsigned long t = Now - BallsNotFoundAt;
  if (t >= 1700) { BallsNotFoundAt = 0; MachineStateTime = Now; return false; }   // attract from its start
  boolean lit = (t >= 60 && t < 1060 && (t - 60) % 200 < 67) || (t >= 1060 && t < 1360);
  SetText(0, lit ? "    ALL" : "");
  SetText(1, lit ? " BALLS" : "");
  SetText(2, lit ? "  NOT" : "");
  SetText(3, lit ? " FOUND" : "");
  return true;
}

// At power-up, before attract (the owner, 11-12): STRANGE SCIENCE 1986 RPU across the four displays (one row
// across the backbox) for 3 s, then POWERED BY RETRO PIN UPGRADE scrolling through from the right as the
// original's marquees do, a place every 100 ms. The font has no "!" yet (punctuation isn't checked against the
// original's), so it's left off
#define SPLASH_NAME_MS    3000
const char SplashScroll[] = "POWERED BY RETRO PIN UPGRADE";
#define SPLASH_MS         (SPLASH_NAME_MS + 100UL * (28 + sizeof(SplashScroll) - 2))
unsigned long SplashUntil = 0;
boolean ShowScrollStep(const char *text, int step);
void ShowStartupSplash() {
  unsigned long t = Now - (SplashUntil - SPLASH_MS);
  if (t < SPLASH_NAME_MS) {
    SetText(0, "STRANGE");
    SetText(1, "SCIENCE");
    SetText(2, "   1986");
    SetText(3, "    RPU");
    return;
  }
  ShowScrollStep(SplashScroll, (t - SPLASH_NAME_MS) / 100);
}

void setup() {
  Serial.begin(115200);
  for (unsigned long t0 = millis(); !Serial && millis() - t0 < 1500;);
  unsigned long result = RPU_InitializeMPU(RPU_CMD_PERFORM_MPU_TEST);
  Serial.printf("Strange Science on RPU %d.%d (400-IC, 6803): init 0x%04lX\n", RPU_OS_MAJOR_VERSION,
                RPU_OS_MINOR_VERSION, result);
  // Stop-all at power-up, as the original does (255 at power-up and game start in its logs): the sound board
  // isn't reset with the RP, so a restart or flash would leave whatever it was looping (the owner, 11-12)
  StopSounds();
  RPU_SetupGameSwitches(NUM_SWITCHES_WITH_TRIGGERS, NUM_PRIORITY_SWITCHES_WITH_TRIGGERS, SolenoidAssociatedSwitches);
  RPU_SetFlasherLamps6803(Flashers, sizeof(Flashers));
  RPU_DisableSolenoidStack();
  RPU_SetDisableFlippers(true);
  RPU_SetContinuousSolenoidBit(false, CONTSOL_6803_BACKBOX_OFF);   // backbox lights on (BackboxLit)
  RPU_SetContinuousSolenoidBit(false, CONTSOL_6803_NEON_OFF);      // neon on (NeonLit); attract turns it off
  randomSeed(micros());
  ReadStoredParameters();
  SetupOperatorMenus();
  SplashUntil = 1;   // the splash once the stuck switches are told (see RunAttractMode)
  StuckFrom = 1;     // look for them at the first pass of attract
  LoadHighScores();   // the high score to date is the table's first
  Credits = RPU_ReadByteFromEEProm(RPU_CREDITS_EEPROM_BYTE);
  if (Credits > MaxCredits) {   // never written by this game (the 400-IC came with 71 there): none
    Credits = 0;
    RPU_WriteByteToEEProm(RPU_CREDITS_EEPROM_BYTE, Credits);
  }
  Now = millis();
  StartPinNodeLink();
  SetMachineState(MACHINE_STATE_ATTRACT);
}

// ------------------------------------------------------------------ the machine states (the RPU's layout)
// loop() runs the state's function; each returns the state to run next

// Attract: the original's messages on the displays and its light show; the sound test's code if one runs
int RunAttractMode(int curState, boolean curStateChanged) {
  (void)curState;
  // Never an initials entry left waiting in attract (it would hold off Start, and its 97 loop on)
  if (curStateChanged && EntryActive) {
    EntryActive = false;
    StopSounds();
    SaveHighScores();
  }
  if (StuckFrom == 1) {   // once, 0.17 s after the start's stop-all (see setup)
    if (Now - MachineStateTime < 170) { ShowAttractLamps(); return MACHINE_STATE_ATTRACT; }
    FindStuckSwitches();
  }
  if (ShowStuckSwitches() || ShowBallsNotFound()) { ShowAttractLamps(); return MACHINE_STATE_ATTRACT; }
  if (SplashUntil) {
    if (SplashUntil == 1) SplashUntil = Now + SPLASH_MS;   // after the stuck switches
    if (Now < SplashUntil) ShowStartupSplash();
    else { SplashUntil = 0; MachineStateTime = Now; }   // the attract messages from their start
  }
  if (!SplashUntil) ShowAttractDisplays();
  ShowAttractLamps();
  if (Now < SoundTestUntil) {
    SetText(2, "SOUND");
    RPU_SetDisplay(3, SoundTestCode, true, 1);
  }
  return MACHINE_STATE_ATTRACT;
}

// Diagnostics: the test the phone started
int RunDiagnosticsMode(int curState, boolean curStateChanged) {
  (void)curStateChanged;
  RunTest();
  return curState;
}

// A game starting: StartGame has set it up; on to the first ball
int InitGamePlay(boolean curStateChanged) {
  (void)curStateChanged;
  return MACHINE_STATE_INIT_NEW_BALL;
}

// A ball's start, once the outhole is empty
int InitNewBall(boolean curStateChanged) {
  (void)curStateChanged;
  if (RPU_ReadSingleSwitchState(SW_OUTHOLE)) return MACHINE_STATE_INIT_NEW_BALL;
  StartBall();
  return MACHINE_STATE_NORMAL_GAMEPLAY;
}

// --- play

// The Power Saver goes out once its 4 s after the left outlane are up
void UpdatePowerSaver() {
  if (SaverGoesOutAt && Now >= SaverGoesOutAt) {
    SaverLit = false;
    SaverGoesOutAt = 0;
    Serial.println("power saver: out");
  }
}

// The serves: the pins down after a release, the next ball after a load, the shooter kick, the ball's
// serve after its intro, and the trough's kicks
void UpdateServing() {
  if (PinsDownAt && Now >= PinsDownAt) { PinsDownAt = 0; if (!BallsLocked) LowerPins(); }
  if (LoadServeAt && Now >= LoadServeAt) {   // the next ball after a load
    LoadServeAt = 0;
    if (!BallsInPlay && !Tilted) { PlaySound(LoadServeSound); ServeKickAt = Now + 50; LoadServed = true; }
  }
  // The shooter kick 67-83 ms after the serve sound (the coil follows the call by about 33 ms; bench)
  if (ServeKickAt && Now >= ServeKickAt) { ServeKickAt = 0; RequestServe(); }
  ServeAfterIntro();
  ServeBall();
}

// The letters rotate; the bubble value cycles (held for a moment after it's made)
void UpdateRotations() {
  if (Now >= NextSweep) SweepWords();
  if (Now >= NextBubble) { NextBubble = Now + 333; BubbleIndex = (BubbleIndex + 1) % 4; }
}

// Awards that come a moment after their switch: L-A-B's 10K, the release's (ALL ATOMS RELEASED), and the
// smash's timed Extra Ball running out (29)
void UpdateTimedAwards() {
  if (LabScoreAt && Now >= LabScoreAt) { LabScoreAt = 0; AddScore(10000); }
  if (ReleaseAwardAt && Now >= ReleaseAwardAt) {
    ReleaseAwardAt = 0;
    PlaySound(SND_EFFECT);
    AddScoreFlat(ReleaseAward);
    StartMarqueeValue("ALL ATOMS RELEASED", ReleaseAward, 85);   // from 85 ms after the 152 (bench: five)
  }
  if (ExtraBallUntil && Now >= ExtraBallUntil) { ExtraBallUntil = 0; PlaySound(SND_TIMED_EB_OVER); }
}

// Multiball: its sound loop, the volts drone back at one ball, and the five-ball multiball's end
void UpdateMultiball() {
  // Through multiball: 72 eight times about 0.19 s apart every 8.33 s, and 80 every 2 s; an 80 that falls in
  // a 72 run waits until 0.2 s after it (bench)
  if (FiveLoopAt && Now >= FiveLoopAt) {
    FiveLoopAt = 0;
    if (InMultiball()) { MBLoopOn = true; MBLoopHeld = false; MBNo80 = true; NextMB72 = Now; }
  }
  if (!InMultiball()) MBLoopOn = false;
  // Down to one ball: the volts drone again 450 ms after the outhole kick that leaves one (bench: mbdrain,
  // 16 450 ms after the 4th drain's kick; the ball counts home here at the kick). After the smash's
  // two-ball (25) too (the owner; provisional: the delay, the five-ball's)
  if (MBDrone && !InMultiball() && !Tilted) {
    MBDrone = false;
    CurrentBackground = 0;
    DroneFrom = Now + 433;   // and 17 ms in UpdateBackground
  }
  if (MBLoopOn && !Tilted && !MBLoopHeld) {
    if (Now >= NextMB72) {
      NextMB72 = Now + 8330;
      MB72RunEnd = Now + 1530;
      if (PlayMultiplier() > 1 && !TimesShowFrom) TimesShowFrom = Now + 67;
      for (byte i = 0; i < 8; i++) Queue(191UL * i, SND_MB_LOOP_1);
    }
    if (!MBNo80 && Now >= NextMB80 && Now >= MB72RunEnd) {
      if (Now - NextMB80 < 2000) NextMB80 += 2000;
      else NextMB80 = Now + 2000;
      PlaySound(SND_MB_LOOP_2);
    }
  }
  // The five-ball multiball ends when every ball is out of the smasher and one (or none) is left. The
  // pins stay down: the next load traps again.
  if (ReleaseAllMultiball && !ReleaseStep && BallsInPlay <= 1) {
    ReleaseAllMultiball = false;
    LogBalls("multiball over");
  }
}

// No ball on the playfield and none on its way
boolean BallIsOver() {
  return !BallsInPlay && !ServeAt && !ServePending && !LoadServeAt && !ServeKickAt && !ReleaseStep && !ReleaseAllMultiball;
}

// The ball's over: the bonus count next. One that drained before any playfield switch didn't count, and is
// served again
int EndOfBall() {
  if (!BallConfirmed && !Tilted) {
    RequestServe();
    Serial.println("drained before a playfield switch: served again");
    return MACHINE_STATE_NORMAL_GAMEPLAY;
  }
    RPU_SetDisableFlippers(true);
    Queue(508, SND_DRAIN);   // 1.04 s after the outhole closes, 467 ms after the kick (bench; the original sends no stop first)
    PlayerStrange[CurrentPlayer] = StrangeLetters;
    PlayerScience[CurrentPlayer] = ScienceLetters;
    PlayerLab[CurrentPlayer] = LabLetters;
    PlayerSparkAtAG[CurrentPlayer] = SparkAtAntiGravity;
    MarqueeAt = ExtraBallShowAt = AdvanceShowAt = LaneCountAt = 0;
    if (LabScoreAt) { LabScoreAt = 0; AddScore(10000); }
  return MACHINE_STATE_COUNTDOWN_BONUS;
}

int ManageGameMode() {
  UpdatePowerSaver();
  UpdateServing();
  UpdateRotations();
  UpdateTimedAwards();
  UpdateMultiball();
  if (!ServeAt && DroneFrom && Now >= DroneFrom) UpdateBackground();
  int next = BallIsOver() ? EndOfBall() : MACHINE_STATE_NORMAL_GAMEPLAY;
  ShowScores();
  ShowPlayLamps();
  return next;
}

// --- the bonus count
// The original (bench): 113 (queued at the drain), COLLECT BONUS, 89 for each 10K of the bonus times the
// multiplier, and 90; the total slides into the score 0.47-1.0 s after the 90 (see ShowBonusCount). A bonus
// held from the player's last ball (the whole multiplied total) is counted next: HELD BONUS, 121, then 89 for
// each 10K and 90, and it goes into the score about 1.1 s later (bench hold2). With Hold Bonus lit, then 121
// and 44 seven times (BONUS HELD), and this ball's total is held for the next. The neon strobes. None after a
// tilt. Times are from the state's start (t)
unsigned long BonusAddAt = 0, BonusScoreAt = 0, BonusHeldAt = 0, BonusEndAt = 0;
unsigned long BonusCountTotal = 0, BonusCountHeld = 0;
boolean BonusPlanned = false;

// This ball's count. From the outhole kick (0.53 s after it closes): 89 from 3.24 s, 258 ms apart (250/267),
// 90 84 ms after the last or at 3.05 s, the next ball's first 72 1.88 s after the 90 (bench: 25 drains). For
// players 2-4 the score first moves to display 1, a 44 a place 33 ms apart from 2.99 s, and the count follows
// (bench: rmsg-p3). Returns the 90's time
unsigned long QueueBonusCount(unsigned long t, byte move, unsigned long sh) {
  byte n = BonusCountTotal / 10000;
  for (byte i = 0; i < move; i++) Queue(2987 + 33UL * i - t, SND_COUNT);
  for (byte i = 0; i < n; i++) Queue(3243 + sh + 775UL * i / 3 - t, SND_BONUS_COUNT);
  // The 90 33 ms after the last 89, and 25 ms more for each 1K left under the 10Ks (bench: 16 counts)
  unsigned long t90 = sh + (n ? 3243 + 775UL * (n - 1) / 3 + 33 + 25 * ((BonusCountTotal / 1000) % 10) : 3053);
  Queue(t90 - t, SND_BONUS_DONE);
  BonusAddAt = t90 + BONUS_SLIDE_END;   // the total into the score as its slide ends (see ShowBonusCount)
  BonusScoreAt = t90 + 1800;
  BonusEndAt = BonusScoreAt;
  return t90;
}

// A bonus held from the player's last ball, counted next. HELD's words 234 ms after the 121, its 89s from
// 199 ms after them as the first count's (hold2). Players 2-4 first move back (91s 1.57 s after the 90), the
// 121 0.23 s after the last 91, then move forward again (44s from 184 ms after it), HELD 468 ms and the 89s
// 683 ms after it (held2p)
void QueueHeldBonusCount(unsigned long t, unsigned long t90, byte move) {
  byte m = BonusCountHeld / 10000;
  unsigned long first89 = BonusScoreAt + 433;
  BonusHeldFrom = BonusScoreAt + 234;
  if (move) {
    BonusBack1At = t90 + 1566;
    for (byte i = 0; i < move; i++) Queue(BonusBack1At + 33UL * i - t, SND_SCORE_BACK);
    BonusScoreAt = BonusBack1At + 33UL * move + 234;
    BonusFwd2At = BonusScoreAt + 184;
    for (byte i = 0; i < move; i++) Queue(BonusFwd2At + 33UL * i - t, SND_COUNT);
    BonusHeldFrom = BonusScoreAt + 468;
    first89 = BonusScoreAt + 683;
  }
  Queue(BonusScoreAt - t, SND_BONUS_TO_SCORE);
  for (byte i = 0; i < m; i++) Queue(first89 + 775UL * i / 3 - t, SND_BONUS_COUNT);
  unsigned long h90 = m ? first89 + 775UL * (m - 1) / 3 + 33 + 25 * ((BonusCountHeld / 1000) % 10) : first89;
  Queue(h90 - t, SND_BONUS_DONE);
  BonusHeldAt = h90 + BONUS_SLIDE_END;   // as its slide ends (hold2)
  BonusHeldFirst89 = first89;
  BonusHeld90 = h90;
  BonusEndAt = h90 + 1800;   // the next ball's first 72 1.88 s after this 90 (hold2's drain into ball 3)
}

// Hold Bonus lit: 121 and 44 seven times (BONUS HELD); this ball's total is held for the next
void QueueBonusHeldForNext(unsigned long t) {
  unsigned long at = BonusHeldAt ? BonusHeldAt : BonusScoreAt;
  Queue(at - t, SND_BONUS_TO_SCORE);
  for (byte i = 0; i < 7; i++) Queue(at + 16 + 195UL * i - t, SND_COUNT);
  BonusEndAt += 1367;   // the first 72 3.25 s after the 90 (bench: lab, hold2)
}

// Players 2-4: the score moves back to its display 1.57 s after the (last) 90, a 91 a place, and the intro's
// 72 0.37 s after the last 91 (bench: rmsg-p3)
void QueueScoreMoveBack(unsigned long t, unsigned long t90, byte move) {
  BonusBackAt = (BonusHeldAt ? BonusHeldAt - BONUS_SLIDE_END : t90) + 1566;
  if (!move) return;
  for (byte i = 0; i < move; i++) Queue(BonusBackAt + 33UL * i - t, SND_SCORE_BACK);
  BonusEndAt += 17 + 33UL * move;
}

// The count's plan at its start: every sound queued, and when each total goes into the score
void PlanBonusCount(unsigned long t) {
  BonusPlanned = true;
  BonusCountTotal = Tilted ? 0 : 1000UL * BonusK * BonusMultiplier;
  BonusCountHeld = Tilted ? 0 : HeldBonus[CurrentPlayer];
  HeldBonus[CurrentPlayer] = 0;
  byte move = Tilted ? 0 : 7 * CurrentPlayer;   // places the score moves for players 2-4
  unsigned long sh = move ? 33UL * move + 20 : 0;
  BonusHeldAt = 0;
  BonusBack1At = BonusFwd2At = 0;
  unsigned long t90 = QueueBonusCount(t, move, sh);
  if (BonusCountHeld) QueueHeldBonusCount(t, t90, move);
  if (HoldBonusLit && !Tilted) QueueBonusHeldForNext(t);
  QueueScoreMoveBack(t, t90, move);
  StrobeNeon(3000);
  BonusMove = move;
  BonusShowShift = sh;
  BonusShowFrom = Tilted ? 0 : MachineStateTime;
  BonusShowTotal = BonusCountTotal;
  BonusShow90 = t90;
  BonusHeldTotal = BonusCountHeld;
  BonusHoldShow = HoldBonusLit && !Tilted;
}

// The totals into the score as their slides end; a held bonus next, and this ball's held for the next
void AddBonusToScore(unsigned long t) {
  if (BonusAddAt && t >= BonusAddAt) {
    BonusAddAt = 0;
    Scores[CurrentPlayer] += BonusCountTotal;
    CheckReplays();
    BonusK = 0;
  }
  if (!BonusAddAt && BonusScoreAt && t >= BonusScoreAt) {
    BonusScoreAt = 0;
    if (!BonusCountHeld && HoldBonusLit && !Tilted) HeldBonus[CurrentPlayer] = BonusCountTotal;
  }
  if (!BonusScoreAt && BonusHeldAt && t >= BonusHeldAt) {
    BonusHeldAt = 0;
    Scores[CurrentPlayer] += BonusCountHeld;
    CheckReplays();
    if (HoldBonusLit && !Tilted) HeldBonus[CurrentPlayer] = BonusCountTotal;
  }
}

int CountdownBonus(boolean curStateChanged) {
  (void)curStateChanged;
  unsigned long t = Now - MachineStateTime;
  if (t < 50) {
    BonusPlanned = false;
    BonusShowFrom = 0;
    ShowScores();
    ShowPlayLamps();
    return MACHINE_STATE_COUNTDOWN_BONUS;
  }
  if (!BonusPlanned) PlanBonusCount(t);
  AddBonusToScore(t);
  int next = MACHINE_STATE_COUNTDOWN_BONUS;
  if (!BonusScoreAt && !BonusHeldAt && t >= BonusEndAt) {
    MessageUntil = 0;
    BonusShowFrom = 0;
    next = MACHINE_STATE_BALL_OVER;
  }
  ShowScores();
  ShowPlayLamps();
  return next;
}

// --- the game's end

// The lamps after a game: dark, then the attract light show from the 100 (bench: the sweep 66-71 ms after it)
void ShowGameOverLamps(unsigned long t) {
  if (GameOverSoundAt && t >= GameOverSoundAt) {
    AttractShowFrom = MachineStateTime + GameOverSoundAt;
    ShowAttractLamps();
  } else {
    RPU_TurnOffAllLamps();
  }
}

// The match: the scores slide a place left, then the number spins with each 96, a matched player's blinking
void ShowMatchDisplays(unsigned long t) {
  if (t < MATCH_SPIN_AT - MATCH_SLIDE_LEAD) {
    for (byte p = 0; p < 4; p++) if (p < LastNumPlayers) ShowScore(p, LastScores[p]); else RPU_SetDisplayBlank(p, 0);
    return;
  }
  unsigned long k = (t - (MATCH_SPIN_AT - MATCH_SLIDE_LEAD)) / 67 + 1;
  if (k > 5) k = 5;
  // The number on show: a new one with each 96 (just after it), the match from 0.1 s after the last
  byte number = MatchNumber, spins = 0;
  while (spins < 15 && t >= MATCH_SPIN_AT + MatchSpin[spins] + 17) spins++;
  if (t < MATCH_SPIN_AT + 2767 + 67) number = ((MatchNumber / 10 + 3 * spins + 7) % 10) * 10;
  unsigned long blinkAt = MATCH_SPIN_AT + 2767 + 1183 - 182;
  for (byte p = 0; p < 4; p++) {
    char line[8], value[12];
    memset(line, ' ', 7);
    line[7] = 0;
    if (p < LastNumPlayers) {
      snprintf(value, sizeof(value), "%02lu", LastScores[p]);
      int length = strlen(value), end = 6 - k;
      for (int i = 0; i < length; i++) if (end - length + 1 + i >= 0) line[end - length + 1 + i] = value[i];
      boolean dark = false;
      if ((MatchedPlayers >> p) & 1) {
        dark = t >= blinkAt && t < blinkAt + 600;
        blinkAt += 985;
      }
      if (spins && !dark && MatchShown) { line[5] = '0' + number / 10; line[6] = '0' + number % 10; }
    }
    for (byte place = 0; place < 7; place++) RPU_SetDisplaySegments6803(p, place, RPU_GetGlyph6803(line[place]));
  }
}

// The match, a high score's initials, then every display its score and on to attract
int ShowMatchSequence(boolean curStateChanged) {
  (void)curStateChanged;
  unsigned long t = Now - MachineStateTime;
  // A high score's initials: the match goes on until the entry starts, and the game waits for it. (No
  // sums with GameOverSoundAt here: it's 0xFFFFFFFF until the entry ends, and 32-bit sums on it wrap. The
  // machine went to attract 4.3 s after a game that made the table, with the entry's 97 looping, 11-12)
  if (EntryActive) {
    if (ShowEntry(t)) RPU_TurnOffAllLamps();
    else { RPU_TurnOffAllLamps(); ShowMatchDisplays(t); }
    return MACHINE_STATE_MATCH_MODE;
  }
  ShowGameOverLamps(t);
  if (!GameOverSoundAt || t >= GameOverSoundAt + 37) {
    for (byte p = 0; p < 4; p++) ShowScore(p, LastScores[p]);
    return t >= (GameOverSoundAt ? GameOverSoundAt + 4320 : 3000) ? MACHINE_STATE_ATTRACT : MACHINE_STATE_MATCH_MODE;
  }
  ShowMatchDisplays(t);
  return MACHINE_STATE_MATCH_MODE;
}

int RunGamePlayMode(int curState, boolean curStateChanged) {
  switch (curState) {
    case MACHINE_STATE_INIT_GAMEPLAY: return InitGamePlay(curStateChanged);
    case MACHINE_STATE_INIT_NEW_BALL: return InitNewBall(curStateChanged);
    case MACHINE_STATE_NORMAL_GAMEPLAY: return ManageGameMode();
    case MACHINE_STATE_COUNTDOWN_BONUS: return CountdownBonus(curStateChanged);
    case MACHINE_STATE_BALL_OVER: return NextBall();
    case MACHINE_STATE_MATCH_MODE: return ShowMatchSequence(curStateChanged);
  }
  return curState;
}

void loop() {
  Now = millis();

  // The operator menus take the switches while they're open (OperatorMenus reads them itself)
  byte sw;
  if (!Menus.OperatorMenusActive())
    while ((sw = RPU_PullFirstFromSwitchStack()) != SWITCH_STACK_EMPTY) HandleSwitch(sw);

  OutholeKickedBall();
  SaucerKick();
  UpdateBallCounts();
  UpdateRelease();
  // The original pulses the pins' coil for the state they're in every 20 s in a game (9 down, 10 up; none
  // after game over: bench rmsg-p2)
  if (Now >= NextPinRefresh) {
    NextPinRefresh = Now + 20000;
    if (NumPlayers && MachineState != MACHINE_STATE_DIAGNOSTICS && MachineState != MACHINE_STATE_ATTRACT &&
        MachineState != MACHINE_STATE_MATCH_MODE && !ReleaseStep) {
      if (PinsUp) RaisePins(); else LowerPins();
      // In multiball with none locked the down pulse comes with 26, 83 ms after it, until a ball drains (bench:
      // bubx12 after the release has it, bubx3 with atoms still locked doesn't; mbdrain, a ball drained 4.7 s
      // before, doesn't either. Provisional: once any ball has drained)
      if (!PinsUp && !BallsLocked && InMultiball() && !MBDrained && MachineState == MACHINE_STATE_NORMAL_GAMEPLAY && !Tilted) {
        Queue(83, SND_MB_RELEASE_2);
        MBDrone = true;
      }
    }
  }
  if (PowerUpPinsLeft && Now >= PowerUpPinsAt) {
    LowerPins();
    PowerUpPinsAt = Now + 100;
    PowerUpPinsLeft--;
  }
  if (StartPinsDownAt && Now >= StartPinsDownAt) { StartPinsDownAt = 0; if (!BallsLocked) LowerPins(); }
  if (PinsAgainAt && Now >= PinsAgainAt) {   // the first load's second pulse
    PinsAgainAt = 0;
    if (PinsUp && ReleaseStep != RELEASE_DROPPED && MachineState != MACHINE_STATE_DIAGNOSTICS) RaisePins();   // load 5's too, with its release waiting
  }

  // The state's own function, as the RPU's games run them
  static int lastState = -1;
  boolean stateChanged = (MachineState != lastState);
  lastState = MachineState;
  int newState = MachineState;
  if (Menus.OperatorMenusActive()) RunOperatorMenu();
  else if (MachineState == MACHINE_STATE_ATTRACT) newState = RunAttractMode(MachineState, stateChanged);
  else if (MachineState == MACHINE_STATE_DIAGNOSTICS) newState = RunDiagnosticsMode(MachineState, stateChanged);
  else newState = RunGamePlayMode(MachineState, stateChanged);
  if (newState != MachineState) SetMachineState(newState);

  static char line[24];
  static int length = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') { if (length) { line[length] = 0; Command(line); length = 0; } }
    else if (length < (int)sizeof(line) - 1) line[length++] = c;
  }

  static unsigned long lastReport = 0;
  if (Now - lastReport > 10000) {
    lastReport = Now;
    Serial.printf("longest interrupt %lu us\n", (unsigned long)RPU_6803_MaxTickUs);
    RPU_6803_MaxTickUs = 0;
  }

  SetSaverArmed();
  UpdateEventShows();
  UpdateQueue();
  UpdateLetterSounds();
  UpdateNeonAndBackbox();
  UpdatePinNode();
  RPU_Update(Now);
}
