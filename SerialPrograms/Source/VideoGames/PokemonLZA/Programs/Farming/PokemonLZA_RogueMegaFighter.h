/*  Rogue Mega Fighter
 *
 *  From: https://github.com/PokemonAutomation/
 *
 *  Fights one Hyperspace Rogue Mega Arena boss with a fixed setup:
 *  a setup Pokemon (Trick-or-Treat + Light Screen), then the main Pokemon
 *  (Screech / Bulk Up / Focus Energy, then plus Rage Fist).
 *  Movement (strafing and dodging) is picked from a small set of styles and
 *  the program learns over many fights which style takes the least damage.
 *
 */

#ifndef PokemonAutomation_PokemonLZA_RogueMegaFighter_H
#define PokemonAutomation_PokemonLZA_RogueMegaFighter_H

#include "Common/Cpp/Options/BooleanCheckBoxOption.h"
#include "Common/Cpp/Options/SimpleIntegerOption.h"
#include "CommonFramework/Notifications/EventNotificationsTable.h"
#include "NintendoSwitch/NintendoSwitch_SingleSwitchProgram.h"

namespace PokemonAutomation{
namespace NintendoSwitch{
namespace PokemonLZA{


class RogueMegaFighter_Descriptor : public SingleSwitchProgramDescriptor{
public:
    RogueMegaFighter_Descriptor();

    class Stats;
    virtual std::unique_ptr<StatsTracker> make_stats() const override;
};


class RogueMegaFighter : public SingleSwitchProgramInstance{
public:
    using Descriptor = RogueMegaFighter_Descriptor;
    RogueMegaFighter();

    virtual void program(SingleSwitchProgramEnvironment& env, ProControllerContext& context) override;

private:
    BooleanCheckBoxOption USE_SETUP_POKEMON;
    SimpleIntegerOption<uint16_t> FOCUS_ENERGY_SECONDS;
    BooleanCheckBoxOption LEARN_MOVEMENT;
    SimpleIntegerOption<uint16_t> MAX_FIGHT_MINUTES;

    EventNotificationsOption NOTIFICATIONS;
};



}
}
}
#endif
