/*  Rogue Mega Fighter
 *
 *  From: https://github.com/PokemonAutomation/
 *
 */

#include <algorithm>
#include <cmath>
#include <sstream>
#include "Common/Cpp/Time.h"
#include "Common/Cpp/Filesystem/FileIO.h"
#include "Common/Cpp/Filesystem/Filesystem.h"
#include "Common/Cpp/PrettyPrint.h"
#include "CommonFramework/GlobalAutoPaths.h"
#include "CommonFramework/Exceptions/OperationFailedExceptionWithScreenshot.h"
#include "CommonFramework/ProgramStats/StatsTracking.h"
#include "CommonFramework/Notifications/ProgramNotifications.h"
#include "CommonFramework/VideoPipeline/VideoFeed.h"
#include "CommonFramework/VideoPipeline/VideoOverlay.h"
#include "NintendoSwitch/Commands/NintendoSwitch_Commands_PushButtons.h"
#include "NintendoSwitch/Commands/NintendoSwitch_Commands_Superscalar.h"
#include "NintendoSwitch/Programs/NintendoSwitch_GameEntry.h"
#include "ML/Inference/ML_YOLOv5Detector.h"
#include "Pokemon/Pokemon_Strings.h"
#include "PokemonLZA_RogueMegaFighter.h"

namespace PokemonAutomation{
namespace NintendoSwitch{
namespace PokemonLZA{

using namespace Pokemon;
using namespace std::chrono_literals;


RogueMegaFighter_Descriptor::RogueMegaFighter_Descriptor()
    : SingleSwitchProgramDescriptor(
        "PokemonLZA:RogueMegaFighter",
        STRING_POKEMON + " LZA", "Rogue Mega Fighter",
        "",
        "Fight a Hyperspace Rogue Mega Arena boss with Gourgeist (Trick-or-Treat, Light Screen) "
        "and Annihilape (Screech, Bulk Up, Focus Energy, plus Rage Fist). "
        "Learns over many fights which way of moving takes the least damage.",
        PanelQualityStanding::THIRD_PARTY_UNVETTED,
        ProgramControllerClass::StandardController_NoRestrictions,
        FeedbackType::REQUIRED,
        AllowCommandsWhenRunning::DISABLE_COMMANDS
    )
{}
class RogueMegaFighter_Descriptor::Stats : public StatsTracker{
public:
    Stats()
        : fights(m_stats["Fights"])
        , hits_taken(m_stats["Hits Taken"])
        , dodges(m_stats["Dodges"])
        , attacks(m_stats["Moves Used"])
    {
        m_display_order.emplace_back("Fights");
        m_display_order.emplace_back("Moves Used");
        m_display_order.emplace_back("Dodges");
        m_display_order.emplace_back("Hits Taken");
    }
    std::atomic<uint64_t>& fights;
    std::atomic<uint64_t>& hits_taken;
    std::atomic<uint64_t>& dodges;
    std::atomic<uint64_t>& attacks;
};
std::unique_ptr<StatsTracker> RogueMegaFighter_Descriptor::make_stats() const{
    return std::unique_ptr<StatsTracker>(new Stats());
}


RogueMegaFighter::RogueMegaFighter()
    : USE_SETUP_POKEMON(
        "<b>Use setup " + STRING_POKEMON + " first:</b><br>"
        "Send out the " + STRING_POKEMON + " to the right of your lead (D-pad right, up), use its left move "
        "(Trick-or-Treat) and right move (Light Screen), then swap back to the lead (D-pad left, up).",
        LockMode::LOCK_WHILE_RUNNING,
        true
    )
    , FOCUS_ENERGY_SECONDS(
        "<b>Re-use Focus Energy every (seconds):</b><br>"
        "Focus Energy doesn't show on screen, so it is refreshed on a timer.",
        LockMode::LOCK_WHILE_RUNNING,
        25, 5, 600
    )
    , LEARN_MOVEMENT(
        "<b>Learn movement:</b><br>"
        "Try different ways of moving and dodging, and over many fights prefer the ones that take the "
        "least damage. What it learned is saved in RogueMegaFighter-Learning.txt next to the program.",
        LockMode::LOCK_WHILE_RUNNING,
        true
    )
    , MAX_FIGHT_MINUTES(
        "<b>Give up after (minutes):</b>",
        LockMode::LOCK_WHILE_RUNNING,
        6, 1, 30
    )
    , SAVE_FRAMES(
        "<b>Save training frames:</b><br>"
        "Save a screenshot every 3 seconds during the fight to RogueMegaFighter-Frames, "
        "to train the boss detector further.",
        LockMode::LOCK_WHILE_RUNNING,
        true
    )
    , NOTIFICATIONS({
        &NOTIFICATION_PROGRAM_FINISH,
        &NOTIFICATION_ERROR_FATAL,
    })
{
    PA_ADD_OPTION(USE_SETUP_POKEMON);
    PA_ADD_OPTION(FOCUS_ENERGY_SECONDS);
    PA_ADD_OPTION(LEARN_MOVEMENT);
    PA_ADD_OPTION(MAX_FIGHT_MINUTES);
    PA_ADD_OPTION(SAVE_FRAMES);
    PA_ADD_OPTION(NOTIFICATIONS);
}



//
//  Reading the battle HUD.
//  All positions are fractions of the screen, measured on a 1920x1080 capture.
//

namespace{

struct Rgb{ int r, g, b; };
Rgb rgb_at(const ImageViewRGB32& image, size_t x, size_t y){
    uint32_t p = image.pixel(x, y);
    return {(int)((p >> 16) & 0xff), (int)((p >> 8) & 0xff), (int)(p & 0xff)};
}

//  Health bar: returns the filled fraction, or -1 if the bar isn't on screen.
//  Filled = bright saturated green/yellow/red. Empty track = dark.
double read_bar(const ImageViewRGB32& image, double x0, double x1, double y0, double y1){
    double W = (double)image.width(), H = (double)image.height();
    size_t X0 = (size_t)(x0 * W), X1 = (size_t)(x1 * W);
    size_t Y0 = (size_t)(y0 * H), Y1 = (size_t)(y1 * H);
    if (X1 <= X0 + 10 || Y1 <= Y0){
        return -1;
    }
    size_t columns = X1 - X0, rows = Y1 - Y0;
    size_t filled = 0, valid = 0;
    size_t edge = std::max<size_t>(2, columns / 100);
    bool edge_filled = true;
    for (size_t x = X0; x < X1; x++){
        size_t f = 0, d = 0;
        for (size_t y = Y0; y < Y1; y++){
            Rgb c = rgb_at(image, x, y);
            int mx = std::max({c.r, c.g, c.b});
            int mn = std::min({c.r, c.g, c.b});
            if (mx > 170 && mx - mn > 110){
                f++;
            }else if (mx < 110){
                d++;
            }
        }
        bool is_filled = f * 10 > rows * 6;
        bool is_dark = d * 10 > rows * 6;
        filled += is_filled;
        valid += is_filled || is_dark;
        if (x - X0 < edge && !is_filled){
            edge_filled = false;
        }
    }
    if (!edge_filled || valid * 10 < columns * 9){
        return -1;
    }
    return (double)filled / (double)columns;
}

//  Stat-change marker: '+' raised (red up arrow), '-' lowered (blue down arrow),
//  '.' unchanged (white dot), '?' can't tell.
char read_marker(const ImageViewRGB32& image, double cx, double cy){
    long W = (long)image.width(), H = (long)image.height();
    long x = (long)(cx * (double)W), y = (long)(cy * (double)H);
    long r = std::max<long>(3, (long)(0.0045 * (double)W));
    size_t red = 0, cyan = 0, white = 0;
    for (long yy = y - r; yy <= y + r; yy++){
        for (long xx = x - r; xx <= x + r; xx++){
            if (xx < 0 || yy < 0 || xx >= W || yy >= H){
                continue;
            }
            Rgb c = rgb_at(image, (size_t)xx, (size_t)yy);
            red   += c.r > 170 && c.g < 120 && c.r - c.b > 70;    //  > 70 so magenta arena flames don't count.
            cyan  += c.r < 110 && c.g > 150 && c.b > 170;
            white += c.r > 200 && c.g > 170 && c.b > 200;
        }
    }
    size_t best = std::max({red, cyan, white});
    if (best < 4){
        return '?';
    }
    return best == red ? '+' : best == cyan ? '-' : '.';
}

//  Number of rows in the battle log (right side) that have a red frame.
//  Red-framed entries are about the boss, e.g. "Ampharos used Breaking Swipe!".
size_t count_boss_log_rows(const ImageViewRGB32& image){
    double W = (double)image.width(), H = (double)image.height();
    size_t X0 = (size_t)(0.667 * W), X1 = (size_t)(0.792 * W);
    size_t Y0 = (size_t)(0.26 * H), Y1 = (size_t)(0.926 * H);
    size_t need = std::max<size_t>(2, (X1 - X0) / 30);
    size_t rows = 0;
    for (size_t y = Y0; y < Y1; y++){
        size_t red = 0;
        for (size_t x = X0; x < X1; x++){
            Rgb c = rgb_at(image, x, y);
            red += c.r > 170 && c.g < 90 && c.r - c.b > 60;
        }
        rows += red >= need;
    }
    return rows;
}

struct Hud{
    double boss_hp;     //  -1 = boss bar not on screen
    double my_hp;       //  -1 = own bar not on screen
    char my_markers[5];     //  Atk, Def, SpA, SpD, Spe
    char boss_markers[5];
    size_t boss_log_rows;
};
Hud read_hud(const ImageViewRGB32& image){
    static const double MY_X[] = {405, 431, 458, 484, 511};
    static const double BOSS_X[] = {893, 927, 960, 993, 1026};
    Hud hud;
    hud.boss_hp = read_bar(image, 633 / 1920., 1287 / 1920., 134 / 1080., 150 / 1080.);
    hud.my_hp   = read_bar(image, 233 / 1920.,  521 / 1920., 878 / 1080., 886 / 1080.);
    for (int i = 0; i < 5; i++){
        hud.my_markers[i]   = read_marker(image, MY_X[i] / 1920, 806 / 1080.);
        hud.boss_markers[i] = read_marker(image, BOSS_X[i] / 1920, 197 / 1080.);
    }
    hud.boss_log_rows = count_boss_log_rows(image);
    return hud;
}

//  Keeps the last seen marker per stat. A row that is fully hidden for a
//  few reads means no stat is changed (the game hides the row then).
class MarkerRow{
public:
    void update(const char markers[5]){
        int seen = 0;
        for (int i = 0; i < 5; i++){
            if (markers[i] != '?'){
                m_known[i] = markers[i];
                seen++;
            }
        }
        m_hidden_reads = seen == 0 ? m_hidden_reads + 1 : 0;
        if (m_hidden_reads >= 3){
            for (char& c : m_known){
                c = '.';
            }
        }
    }
    char operator[](int index) const{ return m_known[index]; }
    std::string str() const{ return std::string(m_known, 5); }

private:
    char m_known[5] = {'.', '.', '.', '.', '.'};
    int m_hidden_reads = 0;
};



//
//  Movement styles and learning.
//

struct MovementStyle{
    const char* name;
    Milliseconds strafe_switch;     //  How often to swap strafe direction.
    Milliseconds roll_every;        //  0 = no timed dodge rolls.
    bool roll_on_boss_move;         //  Dodge when a new boss entry shows up in the battle log.
};
const MovementStyle STYLES[] = {
    {"slow strafe",                 3000ms,    0ms, false},
    {"fast strafe",                 1500ms,    0ms, false},
    {"slow strafe + timed rolls",   3000ms, 3000ms, false},
    {"fast strafe + timed rolls",   1500ms, 3000ms, false},
    {"slow strafe + react rolls",   3000ms,    0ms, true},
    {"fast strafe + react rolls",   1500ms,    0ms, true},
};
constexpr size_t STYLE_COUNT = sizeof(STYLES) / sizeof(STYLES[0]);
const char* LEARNING_FILE = "RogueMegaFighter-Learning.txt";
const char* FIGHT_LOG_FILE = "RogueMegaFighter-Log.txt";

//  Per style: how many 15-second stretches it was used, and the total
//  fraction of HP lost during them. Picks styles with UCB1 (lowest damage,
//  with a bonus for styles that haven't been tried much).
class MovementLearner{
public:
    void load(){
        std::string text;
        if (!file_to_string(LEARNING_FILE, text)){
            return;
        }
        std::istringstream in(text);
        size_t index;
        double count, damage;
        while (in >> index >> count >> damage){
            if (index < STYLE_COUNT){
                m_count[index] = count;
                m_damage[index] = damage;
            }
        }
    }
    void save() const{
        std::ostringstream out;
        for (size_t i = 0; i < STYLE_COUNT; i++){
            out << i << " " << m_count[i] << " " << m_damage[i] << "   # " << STYLES[i].name << "\n";
        }
        string_to_file(LEARNING_FILE, out.str());
    }
    void record(size_t style, double damage){
        m_count[style] += 1;
        m_damage[style] += damage;
    }
    size_t pick() const{
        double total = 0;
        for (size_t i = 0; i < STYLE_COUNT; i++){
            if (m_count[i] == 0){
                return i;
            }
            total += m_count[i];
        }
        size_t best = 0;
        double best_score = 1e9;
        for (size_t i = 0; i < STYLE_COUNT; i++){
            double mean = m_damage[i] / m_count[i];
            double score = mean - 0.05 * std::sqrt(2 * std::log(total) / m_count[i]);
            if (score < best_score){
                best_score = score;
                best = i;
            }
        }
        return best;
    }
    std::string summary() const{
        std::ostringstream out;
        for (size_t i = 0; i < STYLE_COUNT; i++){
            out << "\n- " << STYLES[i].name << ": " << m_count[i] << " tries";
            if (m_count[i] > 0){
                out << ", avg " << (100 * m_damage[i] / m_count[i]) << "% HP lost per 15s";
            }
        }
        return out.str();
    }

private:
    double m_count[STYLE_COUNT] = {};
    double m_damage[STYLE_COUNT] = {};
};


//  Lock on (hold ZL) and use one move. Top = X, Left = Y, Right = A, Bottom = B.
//  The move is pressed 400ms after ZL so the lock-on and move palette are up first;
//  pressed together, Y would dodge instead of using the left move.
void use_move(ProControllerContext& context, Button move, bool plus){
    ssf_press_button(context, BUTTON_ZL, 400ms, 1200ms, 200ms);
    if (plus){
        ssf_press_button(context, BUTTON_PLUS, 320ms, 840ms);
    }
    pbf_press_button(context, move, 160ms, 320ms);
}


}   //  namespace



void RogueMegaFighter::program(SingleSwitchProgramEnvironment& env, ProControllerContext& context){
    RogueMegaFighter_Descriptor::Stats& stats = env.current_stats<RogueMegaFighter_Descriptor::Stats>();

    require_player(env.console, context, BUTTON_NONE);

    MovementLearner learner;
    learner.load();

    //  Wait for the boss health bar.
    env.console.overlay().add_log("Waiting for the boss...");
    WallClock wait_start = current_time();
    while (true){
        context.wait_for_all_requests();
        if (read_hud(env.console.video().snapshot()).boss_hp >= 0){
            break;
        }
        if (current_time() - wait_start > 3min){
            OperationFailedExceptionWithScreenshot::fire(
                ErrorReportMode::NO_ERROR_REPORT,
                "No Rogue Mega health bar seen for 3 minutes. Start the program inside the arena.",
                env.console
            );
        }
        context.wait_for(500ms);
    }
    env.log("Boss health bar found. Fight starts.", COLOR_BLUE);
    stats.fights++;
    env.update_stats();

    const WallClock fight_start = current_time();

    //  D-pad left/right selects a party member, up sends it out (swapping out whoever is out).
    if (USE_SETUP_POKEMON){
        env.console.overlay().add_log("Setup: Trick-or-Treat, Light Screen");
        pbf_press_dpad(context, DPAD_RIGHT, 160ms, 500ms);
        pbf_press_dpad(context, DPAD_UP, 160ms, 2000ms);
        use_move(context, BUTTON_Y, false);     //  Trick-or-Treat
        pbf_wait(context, 2500ms);
        use_move(context, BUTTON_A, false);     //  Light Screen
        pbf_wait(context, 2500ms);
        pbf_press_dpad(context, DPAD_LEFT, 160ms, 500ms);
        stats.attacks += 2;
        env.update_stats();
    }
    pbf_press_dpad(context, DPAD_UP, 160ms, 2000ms);    //  Send out the lead.
    context.wait_for_all_requests();    //  Button presses are queued; wait until they have actually happened.

    MarkerRow mine, boss;
    //  Default-constructed = 1970, so "now - last" is large. (WallClock::min() would overflow.)
    WallClock last_try[4] = {};
    WallClock last_focus_energy = {};
    WallClock last_roll = {};
    WallClock last_me_seen = current_time();
    WallClock last_boss_seen = current_time();  //  Reset here: the setup above takes ~10 seconds.

    size_t style = LEARN_MOVEMENT ? learner.pick() : 0;
    WallClock segment_start = current_time();
    double segment_damage = 0;
    double total_damage = 0;
    double last_hp = -1;
    double last_boss_hp = 1;
    size_t last_log_rows = 0;
    env.log(std::string("Movement style: ") + STYLES[style].name);
    env.console.overlay().add_log(std::string("Moving: ") + STYLES[style].name);

    //  Boss detector (trained on fight recordings). Optional: without the file the program
    //  just strafes without keeping distance.
    const std::string BOSS_MODEL = "PokemonLZA/RogueMegaBoss.onnx";
    std::unique_ptr<ML::YOLOv5Detector> boss_finder;
    if (Filesystem::exists(RESOURCE_PATH() + BOSS_MODEL)){
        boss_finder = std::make_unique<ML::YOLOv5Detector>(BOSS_MODEL);
        env.log("Boss detector loaded.", COLOR_BLUE);
    }else{
        env.log("No boss detector found at Resources/" + BOSS_MODEL + ". Not keeping distance.", COLOR_ORANGE);
    }

    //  Save a frame every few seconds as training material for the boss detector.
    const std::string FRAME_FOLDER = "RogueMegaFighter-Frames/";
    if (SAVE_FRAMES){
        Filesystem::create_directories(FRAME_FOLDER);
    }
    WallClock last_frame_saved = {};

    enum Slot{ TOP, LEFT, RIGHT, BOTTOM };
    static const Button SLOT_BUTTON[] = {BUTTON_X, BUTTON_Y, BUTTON_A, BUTTON_B};
    const Milliseconds retry_gap = 5000ms;

    while (true){
        context.wait_for_all_requests();
        WallClock now = current_time();
        VideoSnapshot screen = env.console.video().snapshot();
        Hud hud = read_hud(screen);

        if (now - fight_start > std::chrono::minutes((uint16_t)MAX_FIGHT_MINUTES)){
            OperationFailedExceptionWithScreenshot::fire(
                ErrorReportMode::NO_ERROR_REPORT,
                "The fight took too long. Stopping.",
                env.console
            );
        }

        //  Fight over: boss bar gone for 10 seconds. Big hits hide it for a few seconds,
        //  so stop pressing buttons after 3 seconds but only end after 10.
        if (hud.boss_hp >= 0){
            last_boss_seen = now;
            last_boss_hp = hud.boss_hp;
        }
        if (now - last_boss_seen > 10s){
            break;
        }
        if (now - last_boss_seen > 3s){
            context.wait_for(500ms);
            continue;
        }

        //  Damage taken (main Pokemon only; setup is done by now).
        if (hud.my_hp >= 0){
            if (last_hp >= 0 && hud.my_hp < last_hp - 0.015){
                double lost = last_hp - hud.my_hp;
                segment_damage += lost;
                total_damage += lost;
                stats.hits_taken++;
                env.update_stats();
                env.log("Took a hit: -" + std::to_string((int)(100 * lost)) + "% HP", COLOR_ORANGE);
            }
            last_hp = hud.my_hp;
            last_me_seen = now;
        }
        //  Main Pokemon fainted: its HP was nearly gone and its bar has disappeared.
        if (last_hp >= 0 && last_hp < 0.15 && now - last_me_seen > 5s){
            break;
        }

        //  New boss entry in the log = the boss just used a move.
        bool boss_moved = hud.boss_log_rows > last_log_rows + std::max<size_t>(4, screen->height() / 70);
        last_log_rows = hud.boss_log_rows;

        mine.update(hud.my_markers);
        boss.update(hud.boss_markers);

        //  Every 15 seconds, score the movement style and pick the next one.
        if (LEARN_MOVEMENT && now - segment_start > 15s){
            learner.record(style, segment_damage);
            learner.save();
            style = learner.pick();
            segment_start = now;
            segment_damage = 0;
            env.log(std::string("Movement style: ") + STYLES[style].name);
            env.console.overlay().add_log(std::string("Moving: ") + STYLES[style].name);
        }
        const MovementStyle& s = STYLES[style];

        //  Pick the move.
        auto can_try = [&](Slot slot){ return now - last_try[slot] > retry_gap; };
        Slot slot;
        bool plus = false;
        if (boss[1] != '-' && can_try(RIGHT)){
            slot = RIGHT;       //  Screech: boss Defense isn't lowered.
        }else if ((mine[0] != '+' || mine[1] != '+') && can_try(LEFT)){
            slot = LEFT;        //  Bulk Up: own Attack or Defense isn't raised.
        }else if (now - last_focus_energy > std::chrono::seconds((uint16_t)FOCUS_ENERGY_SECONDS) && can_try(BOTTOM)){
            slot = BOTTOM;      //  Focus Energy on a timer.
            last_focus_energy = now;
        }else{
            slot = TOP;         //  Plus Rage Fist.
            plus = true;
        }
        if (slot != TOP){
            last_try[slot] = now;
        }
        static const char* SLOT_NAME[] = {"Rage Fist", "Bulk Up", "Screech", "Focus Energy"};
        env.log(
            std::string("Boss HP ") + std::to_string((int)(100 * last_boss_hp)) +
            "%, me " + mine.str() + ", boss " + boss.str() + " -> " + SLOT_NAME[slot]
        );

        if (SAVE_FRAMES && now - last_frame_saved > 3s){
            screen->save(FRAME_FOLDER + now_to_filestring() + ".jpg");
            last_frame_saved = now;
        }

        //  Keep distance: the boss's on-screen height says how close it is.
        //  Too close (contact moves land) -> back off. Too far -> close in.
        double forward = 0;
        if (boss_finder && boss_finder->detect(screen)){
            const ML::YOLOv5Detector::DetectionBox* best = nullptr;
            for (const auto& b : boss_finder->detected_boxes()){
                if (best == nullptr || b.score > best->score){
                    best = &b;
                }
            }
            if (best->box.height > 0.65){
                forward = -1.0;
            }else if (best->box.height < 0.2){
                forward = 0.6;
            }
            env.log("Boss box height " + std::to_string((int)(100 * best->box.height)) + "% -> " +
                (forward < 0 ? "back off" : forward > 0 ? "close in" : "hold distance"));
        }

        //  Strafe for the whole step, dodge if wanted, then lock on and attack.
        double strafe = ((now - fight_start) / s.strafe_switch) % 2 == 0 ? 1.0 : -1.0;
        ssf_press_left_joystick(context, {strafe, forward}, 0ms, 1600ms, 0ms);
        bool roll = (s.roll_on_boss_move && boss_moved) ||
            (s.roll_every > 0ms && now - last_roll > s.roll_every);
        if (roll){
            pbf_press_button(context, BUTTON_Y, 100ms, 300ms);
            last_roll = now;
            stats.dodges++;
        }
        use_move(context, SLOT_BUTTON[slot], plus);
        stats.attacks++;
        env.update_stats();
    }

    if (LEARN_MOVEMENT){
        learner.record(style, segment_damage);
        learner.save();
    }

    bool lost = last_hp >= 0 && last_hp < 0.15;
    std::string result = lost ? "Lost: main " + STRING_POKEMON + " fainted." : "Boss defeated.";
    int seconds = (int)std::chrono::duration_cast<std::chrono::seconds>(current_time() - fight_start).count();
    std::string report =
        result + " Fight took " + std::to_string(seconds) + "s, HP lost " +
        std::to_string((int)(100 * total_damage)) + "%." +
        "\nMovement styles so far:" + learner.summary();
    env.log(report, COLOR_BLUE);

    std::string log_text;
    file_to_string(FIGHT_LOG_FILE, log_text);
    log_text += report + "\n\n";
    string_to_file(FIGHT_LOG_FILE, log_text);

    send_program_finished_notification(env, NOTIFICATION_PROGRAM_FINISH, report);
}



}
}
}
