#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "cata_catch.h"
#include "item.h"
#include "json.h"
#include "json_loader.h"
#include "map_helpers.h"
#include "player_activity.h"
#include "player_helpers.h"
#include "requirements.h"
#include "type_id.h"
#include "wound.h"

static const activity_id ACT_APPLY_PRESSURE( "ACT_APPLY_PRESSURE" );
static const activity_id ACT_TREAT_WOUND( "ACT_TREAT_WOUND" );
static const efftype_id effect_sleep( "sleep" );
static const itype_id itype_rock( "rock" );
static const itype_id itype_water_clean( "water_clean" );
static const skill_id skill_firstaid( "firstaid" );
static const vitamin_id vitamin_blood( "blood" );
static const vitamin_id vitamin_redcells( "redcells" );
static const wound_type_id wound_bleeding_laceration_medium( "bleeding_laceration_medium" );
static const wound_type_id wound_laceration_medium( "laceration_medium" );
static const wound_fix_id wound_fix_pressure_laceration_medium( "pressure_laceration_medium_fix" );

static avatar &pressure_patient()
{
    clear_avatar();
    clear_map_without_vision();
    avatar &who = get_avatar();
    who.set_skill_level( skill_firstaid, 1 );
    who.vitamin_mod( vitamin_blood, -1000 );
    who.vitamin_mod( vitamin_redcells, -1000 );
    return who;
}

static void wound_turn( avatar &who, bool run_activity = true )
{
    calendar::turn += 1_turns;
    who.update_wounds( 1_turns );
    if( run_activity && who.activity ) {
        // Extra speed must not make a clot form sooner.
        who.set_moves( 300 );
        who.activity.do_turn( who );
    }
}

static void start_pressure( avatar &who )
{
    const bodypart_id bp = body_part_hand_r.id();
    const wound &wd = who.get_part( bp )->get_wounds().back();
    who.assign_activity( fix_wound_activity_actor( wound_fix_pressure_laceration_medium->time,
                         bp, wd.type, wound_fix_pressure_laceration_medium, wd.get_uid() ) );
}

TEST_CASE( "direct_pressure_stabilizes_a_wound_without_healing_it", "[wound][bleeding][activity]" )
{
    const std::string suffix = GENERATE( "laceration_medium", "laceration_bad", "scratch_bad",
                                         "cut_medium", "cut_bad" );
    CAPTURE( suffix );
    const wound_type_id bleeding( "bleeding_" + suffix );
    const wound_type_id stable( suffix );
    const wound_fix_id pressure( "pressure_" + suffix + "_fix" );
    avatar &who = pressure_patient();
    const bodypart_id bp = body_part_hand_r.id();
    who.get_part( bp )->add_wound( bleeding );
    const int hp = who.get_part_hp_cur( bp );

    REQUIRE( pressure.is_valid() );
    REQUIRE( pressure->applies_pressure );
    REQUIRE( bleeding->fixes.count( pressure ) == 1 );
    REQUIRE( who.get_greater_skill_or_knowledge_level( skill_firstaid ) == 1 );
    who.assign_activity( fix_wound_activity_actor( pressure->time, bp, bleeding, pressure ) );
    REQUIRE( who.activity.id() == ACT_APPLY_PRESSURE );

    time_duration elapsed = 0_turns;
    while( who.activity && elapsed < pressure->time * 2 ) {
        wound_turn( who );
        elapsed += 1_turns;
    }
    CHECK_FALSE( who.activity );
    CHECK( elapsed >= pressure->time );
    CHECK( who.vitamin_get( vitamin_blood ) == -1000 );
    CHECK( who.vitamin_get( vitamin_redcells ) == -1000 );
    REQUIRE( who.get_part( bp )->has_wound( stable ) );
    CHECK_FALSE( who.get_part( bp )->has_wound( bleeding ) );
    CHECK( who.get_part_hp_cur( bp ) == hp );
    CHECK( who.get_part( bp )->get_wounds().front().get_pain() > 0 );
    CHECK( who.get_part( bp )->get_wounds().front().get_healing_time() ==
           calendar::INDEFINITELY_LONG_DURATION );

    for( int i = 0; i < 600; ++i ) {
        wound_turn( who );
    }
    CHECK( who.vitamin_get( vitamin_blood ) == -1000 );
    CHECK( who.vitamin_get( vitamin_redcells ) == -1000 );
    CHECK( who.get_part( bp )->has_wound( stable ) );
}

TEST_CASE( "interrupting_pressure_restores_bleeding_and_restarts_treatment",
           "[wound][bleeding][activity]" )
{
    avatar &who = pressure_patient();
    who.get_part( body_part_hand_r.id() )->add_wound( wound_bleeding_laceration_medium );
    start_pressure( who );
    REQUIRE( who.activity );
    for( int i = 0; i < 30; ++i ) {
        wound_turn( who );
    }
    REQUIRE( who.activity.moves_left < who.activity.moves_total );
    who.cancel_activity();
    wound_turn( who );
    CHECK( who.vitamin_get( vitamin_blood ) == -1005 );
    CHECK( who.vitamin_get( vitamin_redcells ) == -1005 );
    CHECK_FALSE( who.get_part( body_part_hand_r.id() )->has_wound( wound_laceration_medium ) );

    start_pressure( who );
    REQUIRE( who.activity );
    CHECK( who.activity.moves_left == to_moves<int>( wound_fix_pressure_laceration_medium->time ) );
}

TEST_CASE( "pressure_only_protects_the_selected_wound", "[wound][bleeding][activity]" )
{
    avatar &who = pressure_patient();
    bodypart *right = who.get_part( body_part_hand_r.id() );
    right->add_wound( wound_bleeding_laceration_medium );
    right->add_wound( wound_bleeding_laceration_medium );
    who.get_part( body_part_hand_l.id() )->add_wound( wound_bleeding_laceration_medium );
    const std::int64_t selected = right->get_wounds().back().get_uid();
    REQUIRE( selected != right->get_wounds().front().get_uid() );
    start_pressure( who );
    REQUIRE( who.activity );
    wound_turn( who );
    CHECK( who.vitamin_get( vitamin_blood ) == -1010 );
    CHECK( who.vitamin_get( vitamin_redcells ) == -1010 );

    SECTION( "removing another wound does not change the target" ) {
        right->get_wounds().erase( right->get_wounds().begin() );
        wound_turn( who );
        CHECK( who.vitamin_get( vitamin_blood ) == -1015 );
        CHECK( who.vitamin_get( vitamin_redcells ) == -1015 );
        CHECK( who.activity );
    }
    SECTION( "losing the target does not silently select an identical wound" ) {
        right->get_wounds().pop_back();
        wound_turn( who );
        CHECK_FALSE( who.activity );
        CHECK( who.vitamin_get( vitamin_blood ) == -1020 );
        CHECK( who.vitamin_get( vitamin_redcells ) == -1020 );
        CHECK_FALSE( right->has_wound( wound_laceration_medium ) );
    }
}

TEST_CASE( "pressure_requires_a_free_usable_arm_and_basic_healthcare",
           "[wound][bleeding][activity]" )
{
    avatar &who = pressure_patient();
    who.get_part( body_part_hand_r.id() )->add_wound( wound_bleeding_laceration_medium );

    SECTION( "wielding an item" ) {
        item rock( itype_rock );
        REQUIRE( who.wield( rock ) );
    }
    SECTION( "driving" ) {
        who.controlling_vehicle = true;
    }
    SECTION( "sleeping" ) {
        who.add_effect( effect_sleep, 1_hours );
    }
    SECTION( "the other arm is broken" ) {
        who.set_part_hp_cur( body_part_arm_l.id(), 0 );
    }
    SECTION( "no healthcare skill or knowledge" ) {
        who.set_skill_level( skill_firstaid, 0 );
        who.set_knowledge_level( skill_firstaid, 0 );
    }
    start_pressure( who );
    CHECK_FALSE( who.activity );
    wound_turn( who );
    CHECK( who.vitamin_get( vitamin_blood ) == -1005 );
    CHECK( who.vitamin_get( vitamin_redcells ) == -1005 );
    who.controlling_vehicle = false;
}

TEST_CASE( "losing_the_ability_to_hold_pressure_ends_protection_immediately",
           "[wound][bleeding][activity]" )
{
    avatar &who = pressure_patient();
    who.get_part( body_part_hand_r.id() )->add_wound( wound_bleeding_laceration_medium );
    start_pressure( who );
    REQUIRE( who.activity );
    wound_turn( who );
    REQUIRE( who.vitamin_get( vitamin_blood ) == -1000 );
    who.controlling_vehicle = true;
    wound_turn( who );
    CHECK_FALSE( who.activity );
    CHECK( who.vitamin_get( vitamin_blood ) == -1005 );
    who.controlling_vehicle = false;
}

TEST_CASE( "pressure_and_its_exact_target_survive_serialization",
           "[wound][bleeding][activity][save]" )
{
    avatar &who = pressure_patient();
    bodypart *bp = who.get_part( body_part_hand_r.id() );
    bp->add_wound( wound_bleeding_laceration_medium );
    bp->add_wound( wound_bleeding_laceration_medium );
    const std::int64_t selected = bp->get_wounds().back().get_uid();
    start_pressure( who );
    for( int i = 0; i < 30; ++i ) {
        wound_turn( who );
    }
    const int moves_left = who.activity.moves_left;
    std::ostringstream saved;
    JsonOut jsout( saved );
    jsout.start_object();
    jsout.member( "wounds", bp->get_wounds() );
    jsout.member( "activity", who.activity );
    jsout.end_object();

    who.cancel_activity();
    bp->get_wounds().clear();
    JsonObject data = json_loader::from_string( saved.str() ).get_object();
    data.read( "wounds", bp->get_wounds() );
    data.read( "activity", who.activity );
    REQUIRE( who.activity.id() == ACT_APPLY_PRESSURE );
    CHECK( who.activity.moves_left == moves_left );
    REQUIRE( bp->get_wounds().back().get_uid() == selected );
    wound_turn( who );
    CHECK( who.vitamin_get( vitamin_blood ) == -1155 );
    CHECK( who.vitamin_get( vitamin_redcells ) == -1155 );

    for( int i = 0; who.activity && i < 1200; ++i ) {
        wound_turn( who );
    }
    CHECK_FALSE( who.activity );
    CHECK( bp->get_wounds().size() == 2 );
    CHECK( bp->has_wound( wound_laceration_medium ) );
    CHECK( bp->has_wound( wound_bleeding_laceration_medium ) );
    CHECK( std::none_of( bp->get_wounds().begin(), bp->get_wounds().end(),
    [selected]( const wound & wd ) {
        return wd.get_uid() == selected;
    } ) );
}

TEST_CASE( "ordinary_wound_treatment_does_not_grant_pressure_protection",
           "[wound][bleeding][activity]" )
{
    avatar &who = pressure_patient();
    const bodypart_id bp = body_part_hand_r.id();
    const wound_fix_id treatment( "bleed_laceration_medium_fix" );
    who.set_skill_level( skill_firstaid, 2 );
    who.get_part( bp )->add_wound( wound_bleeding_laceration_medium );
    REQUIRE_FALSE( treatment->applies_pressure );
    who.assign_activity( fix_wound_activity_actor( treatment->time, bp,
                         wound_bleeding_laceration_medium, treatment ) );
    REQUIRE( who.activity.id() == ACT_TREAT_WOUND );
    wound_turn( who );
    CHECK( who.vitamin_get( vitamin_blood ) == -1005 );
    CHECK( who.vitamin_get( vitamin_redcells ) == -1005 );
}

TEST_CASE( "healthcare_one_can_clean_and_recover_after_pressure", "[wound][bleeding][activity]" )
{
    avatar &who = pressure_patient();
    const bodypart_id bp = body_part_hand_r.id();
    who.get_part( bp )->add_wound( wound_bleeding_laceration_medium );
    start_pressure( who );
    for( int i = 0; who.activity && i < 1200; ++i ) {
        wound_turn( who );
    }
    REQUIRE_FALSE( who.activity );
    REQUIRE( who.get_part( bp )->has_wound( wound_laceration_medium ) );

    const wound_fix_id clean( "clean_laceration_medium_fix" );
    const wound_type_id cleaned( "clean_laceration_medium" );
    REQUIRE( clean->skills.at( skill_firstaid ) == 1 );
    item water( itype_water_clean );
    water.charges = 10;
    who.i_add( water );
    REQUIRE( clean->get_requirements().can_make_with_inventory( &who, who.crafting_inventory(),
             is_crafting_component ) );
    who.assign_activity( fix_wound_activity_actor( clean->time, bp, wound_laceration_medium, clean ) );
    for( int i = 0; who.activity && i < 600; ++i ) {
        wound_turn( who );
    }
    REQUIRE_FALSE( who.activity );
    REQUIRE( who.get_part( bp )->has_wound( cleaned ) );
    CHECK( who.vitamin_get( vitamin_blood ) == -1000 );
    CHECK( who.vitamin_get( vitamin_redcells ) == -1000 );
    who.update_wounds( 10_days );
    CHECK_FALSE( who.get_part( bp )->has_wounds() );
}

TEST_CASE( "old_wound_saves_get_distinct_treatment_identifiers", "[wound][save]" )
{
    std::ostringstream old_save;
    JsonOut jsout( old_save );
    jsout.start_object();
    jsout.member( "type", wound_bleeding_laceration_medium );
    jsout.member( "healing_time", calendar::INDEFINITELY_LONG_DURATION );
    jsout.member( "healing_progress", 0.f );
    jsout.member( "pain", 10 );
    jsout.end_object();
    JsonObject data = json_loader::from_string( old_save.str() ).get_object();
    wound first;
    wound second;
    first.deserialize( data );
    second.deserialize( data );
    CHECK( first.get_uid() > 0 );
    CHECK( second.get_uid() > 0 );
    CHECK( first.get_uid() != second.get_uid() );

    avatar &who = pressure_patient();
    bodypart *bp = who.get_part( body_part_hand_r.id() );
    bp->add_wound( first );
    bp->add_wound( first );
    REQUIRE( bp->get_wounds().size() == 2 );
    CHECK( bp->get_wounds().front().get_uid() != bp->get_wounds().back().get_uid() );
}
