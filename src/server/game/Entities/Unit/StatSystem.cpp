/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Unit.h"
#include "DB2Stores.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "Pet.h"
#include "Creature.h"
#include "GameTables.h"
#include "ObjectMgr.h"
#include "SharedDefines.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
#include "World.h"
#include <G3D/g3dmath.h>
#include "ObjectMgr.h"
#include <numeric>

inline bool _ModifyUInt32(bool apply, uint32& baseValue, int32& amount)
{
    // If amount is negative, change sign and value of apply.
    if (amount < 0)
    {
        apply = !apply;
        amount = -amount;
    }
    if (apply)
        baseValue += amount;
    else
    {
        // Make sure we do not get uint32 overflow.
        if (amount > int32(baseValue))
            amount = baseValue;
        baseValue -= amount;
    }
    return apply;
}

/*#######################################
########                         ########
########    UNIT STAT SYSTEM     ########
########                         ########
#######################################*/

void Unit::UpdateAllResistances()
{
    for (uint8 i = SPELL_SCHOOL_NORMAL; i < MAX_SPELL_SCHOOL; ++i)
        UpdateResistances(i);
}

void Unit::UpdateDamagePhysical(WeaponAttackType attType)
{
    float minDamage = 0.0f;
    float maxDamage = 0.0f;

    CalculateMinMaxDamage(attType, false, true, minDamage, maxDamage);

    switch (attType)
    {
        case BASE_ATTACK:
        default:
            SetUpdateFieldStatValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::MinDamage), minDamage);
            SetUpdateFieldStatValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::MaxDamage), maxDamage);
            break;
        case OFF_ATTACK:
            SetUpdateFieldStatValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::MinOffHandDamage), minDamage);
            SetUpdateFieldStatValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::MaxOffHandDamage), maxDamage);
            break;
        case RANGED_ATTACK:
            SetUpdateFieldStatValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::MinRangedDamage), minDamage);
            SetUpdateFieldStatValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::MaxRangedDamage), maxDamage);
            break;
    }
}

/*#######################################
########                         ########
########   PLAYERS STAT SYSTEM   ########
########                         ########
#######################################*/

bool Player::UpdateStats(Stats stat)
{
    // value = ((base_value * base_pct) + total_value) * total_pct
    float value  = GetTotalStatValue(stat);

    SetStat(stat, int32(value));

    switch (stat)
    {
        case STAT_AGILITY:
            UpdateArmor();
            UpdateAllCritPercentages();
            UpdateDodgePercentage();
            break;
        case STAT_STAMINA:
            UpdateMaxHealth();
            break;
        case STAT_INTELLECT:
            UpdateSpellCritChance();
            UpdateArmor();                                  //SPELL_AURA_MOD_RESISTANCE_OF_INTELLECT_PERCENT, only armor currently
            break;
        default:
            break;
    }

    if (stat == STAT_STRENGTH)
        UpdateAttackPowerAndDamage(false);
    else if (stat == STAT_AGILITY)
    {
        UpdateAttackPowerAndDamage(false);
        UpdateAttackPowerAndDamage(true);
    }

    UpdateSpellDamageAndHealingBonus();
    UpdateManaRegen();

    // Update ratings in exist SPELL_AURA_MOD_RATING_FROM_STAT and only depends from stat
    uint32 mask = 0;
    AuraEffectList const& modRatingFromStat = GetAuraEffectsByType(SPELL_AURA_MOD_RATING_FROM_STAT);
    for (AuraEffectList::const_iterator i = modRatingFromStat.begin(); i != modRatingFromStat.end(); ++i)
        if (Stats((*i)->GetMiscValueB()) == stat)
            mask |= (*i)->GetMiscValue();
    if (mask)
    {
        for (uint32 rating = 0; rating < MAX_COMBAT_RATING; ++rating)
            if (mask & (1 << rating))
                ApplyRatingMod(CombatRating(rating), 0, true);
    }
    return true;
}

void Player::ApplySpellPowerBonus(int32 amount, bool apply)
{
    if (HasAuraType(SPELL_AURA_OVERRIDE_SPELL_POWER_BY_AP_PCT))
        return;

    apply = _ModifyUInt32(apply, m_baseSpellPower, amount);

    // For speed just update for client
    ApplyModUpdateFieldValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::ModHealingDonePos), amount, apply);
    for (int i = SPELL_SCHOOL_HOLY; i < MAX_SPELL_SCHOOL; ++i)
        ApplyModDamageDonePos(SpellSchools(i), amount, apply);

    if (HasAuraType(SPELL_AURA_OVERRIDE_ATTACK_POWER_BY_SP_PCT))
    {
        UpdateAttackPowerAndDamage();
        UpdateAttackPowerAndDamage(true);
    }
}

void Player::UpdateSpellDamageAndHealingBonus()
{
    // Magic damage modifiers implemented in Unit::SpellDamageBonusDone
    // This information for client side use only
    // Get healing bonus for all schools
    SetUpdateFieldStatValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::ModHealingDonePos), SpellBaseHealingBonusDone(SPELL_SCHOOL_MASK_ALL));
    // Get damage bonus for all schools
    Unit::AuraEffectList const& modDamageAuras = GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_DONE);
    for (uint16 i = SPELL_SCHOOL_HOLY; i < MAX_SPELL_SCHOOL; ++i)
    {
        SetUpdateFieldValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::ModDamageDoneNeg, i),
            std::accumulate(modDamageAuras.begin(), modDamageAuras.end(), 0, [i](int32 negativeMod, AuraEffect const* aurEff)
        {
            if (aurEff->GetAmount() < 0 && aurEff->GetMiscValue() & (1 << i))
                negativeMod += aurEff->GetAmount();
            return negativeMod;
        }));
        SetUpdateFieldStatValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::ModDamageDonePos, i),
            SpellBaseDamageBonusDone(SpellSchoolMask(1 << i)) - m_activePlayerData->ModDamageDoneNeg[i]);
    }

    if (HasAuraType(SPELL_AURA_OVERRIDE_ATTACK_POWER_BY_SP_PCT))
    {
        UpdateAttackPowerAndDamage();
        UpdateAttackPowerAndDamage(true);
    }

    if (Pet* pet = GetPet())
        pet->UpdateSpellPower();

    if (Guardian* guardian = GetGuardianPet())
        guardian->UpdateSpellPower();

}

bool Player::UpdateAllStats()
{
    for (int8 i = STAT_STRENGTH; i < MAX_STATS; ++i)
    {
        float value = GetTotalStatValue(Stats(i));
        SetStat(Stats(i), int32(value));
    }

    UpdateArmor();
    // calls UpdateAttackPowerAndDamage() in UpdateArmor for SPELL_AURA_MOD_ATTACK_POWER_OF_ARMOR
    UpdateAttackPowerAndDamage(true);
    UpdateMaxHealth();

    for (uint8 i = POWER_MANA; i < MAX_POWERS; ++i)
        UpdateMaxPower(Powers(i));

    UpdateAllRatings();
    UpdateAllCritPercentages();
    UpdateSpellCritChance();
    UpdateBlockPercentage();
    UpdateParryPercentage();
    UpdateDodgePercentage();
    UpdateSpellDamageAndHealingBonus();
    UpdateManaRegen();
    UpdateExpertise(BASE_ATTACK);
    UpdateExpertise(OFF_ATTACK);
    RecalculateRating(CR_ARMOR_PENETRATION);
    UpdateAllResistances();

    if (Pet* pet = GetPet())
        pet->UpdateAllStats();

    if (Guardian* guardian = GetGuardianPet())
        guardian->UpdateAllStats();

    return true;
}

void Player::ApplySpellPenetrationBonus(int32 amount, bool apply)
{
    ApplyModTargetResistance(-amount, apply);
    m_spellPenetrationItemMod += apply ? amount : -amount;
}

void Player::UpdateResistances(uint32 school)
{
    if (school > SPELL_SCHOOL_NORMAL)
    {
        Unit::UpdateResistances(school);

        if (Pet* pet = GetPet())
            pet->UpdateResistances(school);

        if (Guardian* guardian = GetGuardianPet())
            guardian->UpdateResistances(school);
    }
    else
        UpdateArmor();
}

void Player::UpdateArmor()
{
    UnitMods unitMod = UNIT_MOD_ARMOR;

    float value = GetModifierValue(unitMod, BASE_VALUE);    // base armor (from items)
    float baseValue = value;
    value *= GetModifierValue(unitMod, BASE_PCT);           // armor percent from items
    value += GetModifierValue(unitMod, TOTAL_VALUE);

    //add dynamic flat mods
    AuraEffectList const& mResbyIntellect = GetAuraEffectsByType(SPELL_AURA_MOD_RESISTANCE_OF_STAT_PERCENT);
    for (AuraEffectList::const_iterator i = mResbyIntellect.begin(); i != mResbyIntellect.end(); ++i)
    {
        if ((*i)->GetMiscValue() & SPELL_SCHOOL_MASK_NORMAL)
            value += CalculatePct(GetStat(Stats((*i)->GetMiscValueB())), (*i)->GetAmount());
    }

    value *= GetModifierValue(unitMod, TOTAL_PCT);

    SetArmor(int32(baseValue), int32(value - baseValue));

    if (Pet* pet = GetPet())
        pet->UpdateArmor();

    if (Guardian* guardian = GetGuardianPet())
        guardian->UpdateArmor();

    UpdateAttackPowerAndDamage();                           // armor dependent auras update for SPELL_AURA_MOD_ATTACK_POWER_OF_ARMOR
}

float Player::GetHealthBonusFromStamina()
{
    // Taken from PaperDollFrame.lua - 6.0.3.19085
    float ratio = 10.0f;
    if (GtHpPerStaEntry const* hpBase = sHpPerStaGameTable.GetRow(getLevel()))
        ratio = hpBase->Health;

    float stamina = GetStat(STAT_STAMINA);

    return stamina * ratio;
}

void Player::UpdateMaxHealth()
{
    UnitMods unitMod = UNIT_MOD_HEALTH;

    float value = GetModifierValue(unitMod, BASE_VALUE) + GetCreateHealth();
    value *= GetModifierValue(unitMod, BASE_PCT);
    value += GetModifierValue(unitMod, TOTAL_VALUE) + GetHealthBonusFromStamina();
    value *= GetModifierValue(unitMod, TOTAL_PCT);

    SetMaxHealth((uint32)value);

    if (Pet* pet = GetPet())
        pet->UpdateMaxHealth();

    if (Guardian* guardian = GetGuardianPet())
        guardian->UpdateMaxHealth();
}

uint32 Player::GetPowerIndex(Powers power) const
{
    return sDB2Manager.GetPowerIndexByClass(power, getClass());
}

void Player::UpdateMaxPower(Powers power)
{
    uint32 powerIndex = GetPowerIndex(power);
    if (powerIndex == MAX_POWERS || powerIndex >= MAX_POWERS_PER_CLASS)
        return;

    UnitMods unitMod = UnitMods(UNIT_MOD_POWER_START + power);

    float value = GetModifierValue(unitMod, BASE_VALUE) + GetCreatePowers(power);
    value *= GetModifierValue(unitMod, BASE_PCT);
    value += GetModifierValue(unitMod, TOTAL_VALUE);
    value *= GetModifierValue(unitMod, TOTAL_PCT);

    SetMaxPower(power, (int32)std::lroundf(value));
}

void Player::UpdateAttackPowerAndDamage(bool ranged)
{
    float val2 = 0.0f;

    ChrClassesEntry const* entry = sChrClassesStore.AssertEntry(getClass());
    UnitMods unitMod = ranged ? UNIT_MOD_ATTACK_POWER_RANGED : UNIT_MOD_ATTACK_POWER;

    if (!HasAuraType(SPELL_AURA_OVERRIDE_ATTACK_POWER_BY_SP_PCT))
    {
        if (!ranged)
        {
            float strengthValue = std::max(GetStat(STAT_STRENGTH) * entry->AttackPowerPerStrength, 0.0f);
            float agilityValue = std::max(GetStat(STAT_AGILITY) * entry->AttackPowerPerAgility, 0.0f);

            SpellShapeshiftFormEntry const* form = sSpellShapeshiftFormStore.LookupEntry(GetShapeshiftForm());
            // Directly taken from client, SHAPESHIFT_FLAG_AP_FROM_STRENGTH ?
            if (form && form->Flags & 0x20)
                agilityValue += std::max(GetStat(STAT_AGILITY) * entry->AttackPowerPerStrength, 0.0f);

            val2 = strengthValue + agilityValue;
        }
        else
            val2 = (std::max(GetStat(STAT_AGILITY), 0.0f)) * entry->RangedAttackPowerPerAgility;
    }
    else
    {
        int32 minSpellPower = m_activePlayerData->ModHealingDonePos;
        for (int i = SPELL_SCHOOL_HOLY; i < MAX_SPELL_SCHOOL; ++i)
            minSpellPower = std::min(minSpellPower, m_activePlayerData->ModDamageDonePos[i]);

        val2 = CalculatePct(float(minSpellPower), *m_activePlayerData->OverrideAPBySpellPowerPercent);
    }

    SetModifierValue(unitMod, BASE_VALUE, val2);

    float base_attPower  = GetModifierValue(unitMod, BASE_VALUE) * GetModifierValue(unitMod, BASE_PCT);
    float attPowerMod = GetModifierValue(unitMod, TOTAL_VALUE);
    float attPowerMultiplier = GetModifierValue(unitMod, TOTAL_PCT) - 1.0f;

    if (ranged)
    {
        SetRangedAttackPower(int32(base_attPower));
        SetRangedAttackPowerModPos(int32(attPowerMod));
        SetRangedAttackPowerMultiplier(attPowerMultiplier);
    }
    else
    {
        SetAttackPower(int32(base_attPower));
        SetAttackPowerModPos(int32(attPowerMod));
        SetAttackPowerMultiplier(attPowerMultiplier);
    }

    Pet* pet = GetPet();                                //update pet's AP
    //automatically update weapon damage after attack power modification
    if (ranged)
    {
        UpdateDamagePhysical(RANGED_ATTACK);
        if (pet && pet->IsHunterPet()) // At ranged attack change for hunter pet
            pet->UpdateAttackPowerAndDamage(ranged);
    }
    else
    {
        UpdateDamagePhysical(BASE_ATTACK);
        if (Item* offhand = GetWeaponForAttack(OFF_ATTACK, true))
            if (CanDualWield() || offhand->GetTemplate()->GetFlags3() & ITEM_FLAG3_ALWAYS_ALLOW_DUAL_WIELD)
                UpdateDamagePhysical(OFF_ATTACK);

        if (HasAuraType(SPELL_AURA_MOD_SPELL_DAMAGE_OF_ATTACK_POWER) ||
            HasAuraType(SPELL_AURA_MOD_SPELL_HEALING_OF_ATTACK_POWER) ||
            HasAuraType(SPELL_AURA_OVERRIDE_SPELL_POWER_BY_AP_PCT))
            UpdateSpellDamageAndHealingBonus();

        if (pet)
            pet->UpdateAttackPowerAndDamage();

        if (Guardian* guardian = GetGuardianPet())
            guardian->UpdateAttackPowerAndDamage();
    }
}

void Player::CalculateMinMaxDamage(WeaponAttackType attType, bool normalized, bool addTotalPct, float& minDamage, float& maxDamage)
{
    UnitMods unitMod;

    switch (attType)
    {
        case BASE_ATTACK:
        default:
            unitMod = UNIT_MOD_DAMAGE_MAINHAND;
            break;
        case OFF_ATTACK:
            unitMod = UNIT_MOD_DAMAGE_OFFHAND;
            break;
        case RANGED_ATTACK:
            unitMod = UNIT_MOD_DAMAGE_RANGED;
            break;
    }

    float attackPowerMod = std::max(GetAPMultiplier(attType, normalized), 0.25f);

    float baseValue  = GetModifierValue(unitMod, BASE_VALUE) + GetTotalAttackPowerValue(attType) / 3.5f * attackPowerMod;
    float basePct    = GetModifierValue(unitMod, BASE_PCT);
    float totalValue = GetModifierValue(unitMod, TOTAL_VALUE);
    float totalPct   = addTotalPct ? GetModifierValue(unitMod, TOTAL_PCT) : 1.0f;

    float weaponMinDamage = GetWeaponDamageRange(attType, MINDAMAGE);
    float weaponMaxDamage = GetWeaponDamageRange(attType, MAXDAMAGE);

    float versaDmgMod = 1.0f;

    AddPct(versaDmgMod, GetRatingBonusValue(CR_VERSATILITY_DAMAGE_DONE) + float(GetTotalAuraModifier(SPELL_AURA_MOD_VERSATILITY)));

    SpellShapeshiftFormEntry const* shapeshift = sSpellShapeshiftFormStore.LookupEntry(GetShapeshiftForm());
    if (shapeshift && shapeshift->CombatRoundTime)
    {
        weaponMinDamage = weaponMinDamage * shapeshift->CombatRoundTime / 1000.0f / attackPowerMod;
        weaponMaxDamage = weaponMaxDamage * shapeshift->CombatRoundTime / 1000.0f / attackPowerMod;
    }
    else if (!CanUseAttackType(attType)) // check if player not in form but still can't use (disarm case)
    {
        // cannot use ranged/off attack, set values to 0
        if (attType != BASE_ATTACK)
        {
            minDamage = 0;
            maxDamage = 0;
            return;
        }
        weaponMinDamage = BASE_MINDAMAGE;
        weaponMaxDamage = BASE_MAXDAMAGE;
    }

    minDamage = ((weaponMinDamage + baseValue) * basePct + totalValue) * totalPct * versaDmgMod;
    maxDamage = ((weaponMaxDamage + baseValue) * basePct + totalValue) * totalPct * versaDmgMod;
}

void Player::UpdateBlockPercentage()
{
    // No block
    float value = 0.0f;
    if (CanBlock())
    {
        // Base value
        value = 5.0f;
        // Increase from SPELL_AURA_MOD_BLOCK_PERCENT aura
        value += GetTotalAuraModifier(SPELL_AURA_MOD_BLOCK_PERCENT);
        // Increase from rating
        value += GetRatingBonusValue(CR_BLOCK);

        if (sWorld->getBoolConfig(CONFIG_STATS_LIMITS_ENABLE))
             value = value > sWorld->getFloatConfig(CONFIG_STATS_LIMITS_BLOCK) ? sWorld->getFloatConfig(CONFIG_STATS_LIMITS_BLOCK) : value;
    }
    SetUpdateFieldStatValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::BlockPercentage), value);
}

void Player::UpdateCritPercentage(WeaponAttackType attType)
{
    auto applyCritLimit = [](float value)
    {
        if (sWorld->getBoolConfig(CONFIG_STATS_LIMITS_ENABLE))
            value = value > sWorld->getFloatConfig(CONFIG_STATS_LIMITS_CRIT) ? sWorld->getFloatConfig(CONFIG_STATS_LIMITS_CRIT) : value;
        return value;
    };

    switch (attType)
    {
        case OFF_ATTACK:
            SetUpdateFieldStatValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::OffhandCritPercentage),
                applyCritLimit(GetTotalPercentageModValue(OFFHAND_CRIT_PERCENTAGE) + GetRatingBonusValue(CR_CRIT_MELEE)));
            break;
        case RANGED_ATTACK:
            SetUpdateFieldStatValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::RangedCritPercentage),
                applyCritLimit(GetTotalPercentageModValue(RANGED_CRIT_PERCENTAGE) + GetRatingBonusValue(CR_CRIT_RANGED)));
            break;
        case BASE_ATTACK:
        default:
            SetUpdateFieldStatValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::CritPercentage),
                applyCritLimit(GetTotalPercentageModValue(CRIT_PERCENTAGE) + GetRatingBonusValue(CR_CRIT_MELEE)));
            break;
    }
}

void Player::UpdateLeechPercentage()
{
    float value = GetTotalAuraModifier(SPELL_AURA_MOD_LEECH);
    value += GetRatingBonusValue(CR_LIFESTEAL);
    SetUpdateFieldValue(m_values.ModifyValue(&Player::m_unitData).ModifyValue(&UF::UnitData::Lifesteal), value);
}

void Player::UpdateAllCritPercentages()
{
    float value = 5.0f;

    SetBaseModValue(CRIT_PERCENTAGE, PCT_MOD, value);
    SetBaseModValue(OFFHAND_CRIT_PERCENTAGE, PCT_MOD, value);
    SetBaseModValue(RANGED_CRIT_PERCENTAGE, PCT_MOD, value);

    UpdateCritPercentage(BASE_ATTACK);
    UpdateCritPercentage(OFF_ATTACK);
    UpdateCritPercentage(RANGED_ATTACK);
}

void Player::UpdateMastery()
{
    if (!CanUseMastery())
    {
        SetUpdateFieldValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::Mastery), 0.0f);
        return;
    }

    float value = GetTotalAuraModifier(SPELL_AURA_MASTERY);
    value += GetRatingBonusValue(CR_MASTERY);
    SetUpdateFieldValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::Mastery), value);

    ChrSpecializationEntry const* chrSpec = sChrSpecializationStore.LookupEntry(GetPrimarySpecialization());
    if (!chrSpec)
        return;

    for (uint32 i = 0; i < MAX_MASTERY_SPELLS; ++i)
    {
        if (Aura* aura = GetAura(chrSpec->MasterySpellID[i]))
        {
            for (SpellEffectInfo const* effect : aura->GetSpellEffectInfos())
            {
                if (!effect)
                    continue;

                float mult = effect->BonusCoefficient;
                if (G3D::fuzzyEq(mult, 0.0f))
                    continue;

                aura->GetEffect(effect->EffectIndex)->ChangeAmount(int32(value * mult));
            }
        }
    }
}

void Player::UpdateVersatilityDamageDone()
{
    // No proof that CR_VERSATILITY_DAMAGE_DONE is allways = ActivePlayerData::Versatility
    SetUpdateFieldValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::Versatility), m_activePlayerData->CombatRatings[CR_VERSATILITY_DAMAGE_DONE]);

    if (getClass() == CLASS_HUNTER)
        UpdateDamagePhysical(RANGED_ATTACK);
    else
        UpdateDamagePhysical(BASE_ATTACK);
}

void Player::UpdateHealingDonePercentMod()
{
    float value = 1.0f;

    AddPct(value, GetRatingBonusValue(CR_VERSATILITY_HEALING_DONE) + GetTotalAuraModifier(SPELL_AURA_MOD_VERSATILITY));

    for (AuraEffect const* auraEffect : GetAuraEffectsByType(SPELL_AURA_MOD_HEALING_DONE_PERCENT))
        AddPct(value, auraEffect->GetAmount());

    SetUpdateFieldStatValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::ModHealingDonePercent), value);
}

void Player::UpdateAverageItemLevel()
{
    SetUpdateFieldValue(m_values.ModifyValue(&Player::m_playerData).ModifyValue(&UF::PlayerData::AvgItemLevel, PLAYER_AVG_ITEM_LEVEL_EQUIPPED_AND_BAG), GetAverageItemLevelEquippedAndBag());
    SetUpdateFieldValue(m_values.ModifyValue(&Player::m_playerData).ModifyValue(&UF::PlayerData::AvgItemLevel, PLAYER_AVG_ITEM_LEVEL_EQUIPPED), GetAverageItemLevelEquipped());
    // @TODO : Possible old offsets for WoD itemlevel scaling
    //SetFloatValue(PLAYER_FIELD_AVG_ITEM_LEVEL + PlayerAvgItemLevelOffsets::PLAYER_AVG_ITEM_LEVEL_UNK3, equipped);
    //SetFloatValue(PLAYER_FIELD_AVG_ITEM_LEVEL + PlayerAvgItemLevelOffsets::PLAYER_AVG_ITEM_LEVEL_UNK4, equipped_bag);
}

const float m_diminishing_k[MAX_CLASSES] =
{
    0.9560f,  // Warrior
    0.9560f,  // Paladin
    0.9880f,  // Hunter
    0.9880f,  // Rogue
    0.9830f,  // Priest
    0.9560f,  // DK
    0.9880f,  // Shaman
    0.9830f,  // Mage
    0.9830f,  // Warlock
    0.9830f,  // Monk
    0.9720f,  // Druid
    0.9830f   // Demon Hunter
};

void Player::UpdateParryPercentage()
{
    const float parry_cap[MAX_CLASSES] =
    {
        65.631440f,     // Warrior
        65.631440f,     // Paladin
        145.560408f,    // Hunter
        145.560408f,    // Rogue
        0.0f,           // Priest
        65.631440f,     // DK
        145.560408f,    // Shaman
        0.0f,           // Mage
        0.0f,           // Warlock
        90.6425f,       // Monk
        0.0f,           // Druid
        65.631440f      // Demon Hunter
    };

    // No parry
    float value = 0.0f;
    uint32 pclass = getClass()-1;
    if (CanParry() && parry_cap[pclass] > 0.0f)
    {
        float nondiminishing  = 5.0f;
        // Parry from rating
        float diminishing = GetRatingBonusValue(CR_PARRY);
        // Parry from SPELL_AURA_MOD_PARRY_PERCENT aura
        nondiminishing += GetTotalAuraModifier(SPELL_AURA_MOD_PARRY_PERCENT);
        // apply diminishing formula to diminishing parry chance
        value = nondiminishing + diminishing * parry_cap[pclass] / (diminishing + parry_cap[pclass] * m_diminishing_k[pclass]);

        if (sWorld->getBoolConfig(CONFIG_STATS_LIMITS_ENABLE))
             value = value > sWorld->getFloatConfig(CONFIG_STATS_LIMITS_PARRY) ? sWorld->getFloatConfig(CONFIG_STATS_LIMITS_PARRY) : value;

    }
    SetUpdateFieldStatValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::ParryPercentage), value);
}

void Player::UpdateDodgePercentage()
{
    const float dodge_cap[MAX_CLASSES] =
    {
        65.631440f,     // Warrior
        65.631440f,     // Paladin
        145.560408f,    // Hunter
        145.560408f,    // Rogue
        150.375940f,    // Priest
        65.631440f,     // DK
        145.560408f,    // Shaman
        150.375940f,    // Mage
        150.375940f,    // Warlock
        145.560408f,    // Monk
        116.890707f,    // Druid
        145.560408f     // Demon Hunter
    };

    float diminishing = 0.0f, nondiminishing = 0.0f;
    GetDodgeFromAgility(diminishing, nondiminishing);
    // Dodge from SPELL_AURA_MOD_DODGE_PERCENT aura
    nondiminishing += GetTotalAuraModifier(SPELL_AURA_MOD_DODGE_PERCENT);
    // Dodge from rating
    diminishing += GetRatingBonusValue(CR_DODGE);
    // apply diminishing formula to diminishing dodge chance
    uint32 pclass = getClass()-1;
    float value = nondiminishing + (diminishing * dodge_cap[pclass] / (diminishing + dodge_cap[pclass] * m_diminishing_k[pclass]));

    if (sWorld->getBoolConfig(CONFIG_STATS_LIMITS_ENABLE))
         value = value > sWorld->getFloatConfig(CONFIG_STATS_LIMITS_DODGE) ? sWorld->getFloatConfig(CONFIG_STATS_LIMITS_DODGE) : value;

    SetUpdateFieldStatValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::DodgePercentage), value);
}

void Player::UpdateSpellCritChance()
{
    float crit = 5.0f;
    // Increase crit from SPELL_AURA_MOD_SPELL_CRIT_CHANCE
    crit += GetTotalAuraModifier(SPELL_AURA_MOD_SPELL_CRIT_CHANCE);
    // Increase crit from SPELL_AURA_MOD_CRIT_PCT
    crit += GetTotalAuraModifier(SPELL_AURA_MOD_CRIT_PCT);
    // Increase crit from spell crit ratings
    crit += GetRatingBonusValue(CR_CRIT_SPELL);

    // Store crit value
    SetUpdateFieldValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::SpellCritPercentage), crit);
}

void Player::UpdateCorruption()
{
    float effectiveCorruption = GetRatingBonusValue(CR_CORRUPTION) - GetRatingBonusValue(CR_CORRUPTION_RESISTANCE);
    for (CorruptionEffectsEntry const* corruptionEffect : sCorruptionEffectsStore)
    {
        if ((CorruptionEffectsFlag(corruptionEffect->Flags) & CorruptionEffectsFlag::Disabled) != CorruptionEffectsFlag::None)
            continue;

        if (effectiveCorruption < corruptionEffect->MinCorruption)
        {
            RemoveAura(corruptionEffect->Aura);
            continue;
        }

        if (PlayerConditionEntry const* playerCondition = sPlayerConditionStore.LookupEntry(corruptionEffect->PlayerConditionID))
        {
            if (!ConditionMgr::IsPlayerMeetingCondition(this, playerCondition))
            {
                RemoveAura(corruptionEffect->Aura);
                continue;
            }
        }

        CastSpell(this, corruptionEffect->Aura, true);
    }
}

void Player::UpdateArmorPenetration(int32 amount)
{
    // Store Rating Value
    SetUpdateFieldValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::CombatRatings, CR_ARMOR_PENETRATION), amount);
}

void Player::UpdateMeleeHitChances()
{
    m_modMeleeHitChance = 7.5f + (float)GetTotalAuraModifier(SPELL_AURA_MOD_HIT_CHANCE);
    m_modMeleeHitChance += GetRatingBonusValue(CR_HIT_MELEE);
}

void Player::UpdateRangedHitChances()
{
    m_modRangedHitChance = 7.5f + (float)GetTotalAuraModifier(SPELL_AURA_MOD_HIT_CHANCE);
    m_modRangedHitChance += GetRatingBonusValue(CR_HIT_RANGED);
}

void Player::UpdateSpellHitChances()
{
    m_modSpellHitChance = 15.0f + (float)GetTotalAuraModifier(SPELL_AURA_MOD_SPELL_HIT_CHANCE);
    m_modSpellHitChance += GetRatingBonusValue(CR_HIT_SPELL);
}

void Player::UpdateExpertise(WeaponAttackType attack)
{
    if (attack == RANGED_ATTACK)
        return;

    int32 expertise = int32(GetRatingBonusValue(CR_EXPERTISE));

    Item const* weapon = GetWeaponForAttack(attack, true);
    expertise += GetTotalAuraModifier(SPELL_AURA_MOD_EXPERTISE, [weapon](AuraEffect const* aurEff) -> bool
    {
        return aurEff->GetSpellInfo()->IsItemFitToSpellRequirements(weapon);
    });

    if (expertise < 0)
        expertise = 0;

    switch (attack)
    {
        case BASE_ATTACK:
            SetUpdateFieldValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::MainhandExpertise), expertise);
            break;
        case OFF_ATTACK:
            SetUpdateFieldValue(m_values.ModifyValue(&Player::m_activePlayerData).ModifyValue(&UF::ActivePlayerData::OffhandExpertise), expertise);
            break;
        default:
            break;
    }
}

void Player::ApplyManaRegenBonus(int32 amount, bool apply)
{
    _ModifyUInt32(apply, m_baseManaRegen, amount);
    UpdateManaRegen();
}

void Player::ApplyHealthRegenBonus(int32 amount, bool apply)
{
    _ModifyUInt32(apply, m_baseHealthRegen, amount);
}

void Player::UpdateManaRegen()
{
    uint32 manaIndex = GetPowerIndex(POWER_MANA);
    if (manaIndex == MAX_POWERS)
        return;

    // Get base of Mana Pool in sBaseMPGameTable
    uint32 basemana = 0;
    sObjectMgr->GetPlayerClassLevelInfo(getClass(), getLevel(), basemana);
    float base_regen = basemana / 100.f;

    base_regen += GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_POWER_REGEN, POWER_MANA);

    // Apply PCT bonus from SPELL_AURA_MOD_POWER_REGEN_PERCENT
    base_regen *= GetTotalAuraMultiplierByMiscValue(SPELL_AURA_MOD_POWER_REGEN_PERCENT, POWER_MANA);

    // Apply PCT bonus from SPELL_AURA_MOD_MANA_REGEN_PCT
    base_regen *= GetTotalAuraMultiplierByMiscValue(SPELL_AURA_MOD_MANA_REGEN_PCT, POWER_MANA);

    SetUpdateFieldValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::PowerRegenFlatModifier, manaIndex), base_regen);
    SetUpdateFieldValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::PowerRegenInterruptedFlatModifier, manaIndex), base_regen);
}

void Player::UpdateAllRunesRegen()
{
    if (getClass() != CLASS_DEATH_KNIGHT)
        return;

    uint32 runeIndex = GetPowerIndex(POWER_RUNES);
    if (runeIndex == MAX_POWERS)
        return;

    PowerTypeEntry const* runeEntry = sDB2Manager.GetPowerTypeEntry(POWER_RUNES);

    uint32 cooldown = GetRuneBaseCooldown();
    SetUpdateFieldValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::PowerRegenFlatModifier, runeIndex), float(1 * IN_MILLISECONDS) / float(cooldown) - runeEntry->RegenPeace);
    SetUpdateFieldValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::PowerRegenInterruptedFlatModifier, runeIndex), float(1 * IN_MILLISECONDS) / float(cooldown) - runeEntry->RegenCombat);
}

void Player::_ApplyAllStatBonuses()
{
    SetCanModifyStats(false);

    _ApplyAllAuraStatMods();
    _ApplyAllItemMods();
    ApplyAllAzeriteItemMods(true);

    SetCanModifyStats(true);

    UpdateAllStats();
}

void Player::_RemoveAllStatBonuses()
{
    SetCanModifyStats(false);

    ApplyAllAzeriteItemMods(false);
    _RemoveAllItemMods();
    _RemoveAllAuraStatMods();

    SetCanModifyStats(true);

    UpdateAllStats();
}

/*#######################################
########                         ########
########    MOBS STAT SYSTEM     ########
########                         ########
#######################################*/

bool Creature::UpdateStats(Stats /*stat*/)
{
    return true;
}

bool Creature::UpdateAllStats()
{
    UpdateMaxHealth();
    UpdateAttackPowerAndDamage();
    UpdateAttackPowerAndDamage(true);

    for (uint8 i = POWER_MANA; i < MAX_POWERS; ++i)
        UpdateMaxPower(Powers(i));

    UpdateAllResistances();

    return true;
}

void Creature::UpdateArmor()
{
    float baseValue = GetModifierValue(UNIT_MOD_ARMOR, BASE_VALUE);
    float value = GetTotalAuraModValue(UNIT_MOD_ARMOR);
    SetArmor(int32(baseValue), int32(value - baseValue));
}

void Creature::UpdateMaxHealth()
{
    float value = GetTotalAuraModValue(UNIT_MOD_HEALTH);
    SetMaxHealth(uint32(value));
}

uint32 Creature::GetPowerIndex(Powers power) const
{
    if (power == GetPowerType())
        return 0;
    if (power == POWER_ALTERNATE_POWER)
        return 1;
    if (power == POWER_COMBO_POINTS)
        return 2;
    return MAX_POWERS;
}

void Creature::UpdateMaxPower(Powers power)
{
    if (GetPowerIndex(power) == MAX_POWERS)
        return;

    UnitMods unitMod = UnitMods(UNIT_MOD_POWER_START + power);

    float value = GetModifierValue(unitMod, BASE_VALUE) + GetCreatePowers(power);
    value *= GetModifierValue(unitMod, BASE_PCT);
    value += GetModifierValue(unitMod, TOTAL_VALUE);
    value *= GetModifierValue(unitMod, TOTAL_PCT);

    SetMaxPower(power, (int32)std::lroundf(value));
}

void Creature::UpdateAttackPowerAndDamage(bool ranged)
{
    UnitMods unitMod = ranged ? UNIT_MOD_ATTACK_POWER_RANGED : UNIT_MOD_ATTACK_POWER;

    float baseAttackPower       = GetModifierValue(unitMod, BASE_VALUE) * GetModifierValue(unitMod, BASE_PCT);
    float attackPowerMultiplier = GetModifierValue(unitMod, TOTAL_PCT) - 1.0f;

    if (ranged)
    {
        SetRangedAttackPower(int32(baseAttackPower));
        SetRangedAttackPowerMultiplier(attackPowerMultiplier);
    }
    else
    {
        SetAttackPower(int32(baseAttackPower));
        SetAttackPowerMultiplier(attackPowerMultiplier);
    }

    // automatically update weapon damage after attack power modification
    if (ranged)
        UpdateDamagePhysical(RANGED_ATTACK);
    else
    {
        UpdateDamagePhysical(BASE_ATTACK);
        UpdateDamagePhysical(OFF_ATTACK);
    }
}

void Creature::CalculateMinMaxDamage(WeaponAttackType attType, bool normalized, bool addTotalPct, float& minDamage, float& maxDamage)
{
    float variance = 1.0f;
    UnitMods unitMod;
    switch (attType)
    {
        case BASE_ATTACK:
        default:
            variance = GetCreatureTemplate()->BaseVariance;
            unitMod = UNIT_MOD_DAMAGE_MAINHAND;
            break;
        case OFF_ATTACK:
            variance = GetCreatureTemplate()->BaseVariance;
            unitMod = UNIT_MOD_DAMAGE_OFFHAND;
            break;
        case RANGED_ATTACK:
            variance = GetCreatureTemplate()->RangeVariance;
            unitMod = UNIT_MOD_DAMAGE_RANGED;
            break;
    }

    if (attType == OFF_ATTACK && !haveOffhandWeapon())
    {
        minDamage = 0.0f;
        maxDamage = 0.0f;
        return;
    }

    float weaponMinDamage = GetWeaponDamageRange(attType, MINDAMAGE);
    float weaponMaxDamage = GetWeaponDamageRange(attType, MAXDAMAGE);

    if (!CanUseAttackType(attType)) // disarm case
    {
        weaponMinDamage = 0.0f;
        weaponMaxDamage = 0.0f;
    }

    float attackPower      = GetTotalAttackPowerValue(attType);
    float attackSpeedMulti = GetAPMultiplier(attType, normalized);
    float baseValue        = GetModifierValue(unitMod, BASE_VALUE) + (attackPower / 3.5f) * variance;
    float basePct          = GetModifierValue(unitMod, BASE_PCT) * attackSpeedMulti;
    float totalValue       = GetModifierValue(unitMod, TOTAL_VALUE);
    float totalPct         = addTotalPct ? GetModifierValue(unitMod, TOTAL_PCT) : 1.0f;
    float dmgMultiplier    = GetCreatureTemplate()->ModDamage; // = ModDamage * _GetDamageMod(rank);

    minDamage = ((weaponMinDamage + baseValue) * dmgMultiplier * basePct + totalValue) * totalPct;
    maxDamage = ((weaponMaxDamage + baseValue) * dmgMultiplier * basePct + totalValue) * totalPct;
}

/*#######################################
########                         ########
########    PETS STAT SYSTEM     ########
########                         ########
#######################################*/

bool Guardian::UpdateAllStats()
{
    UpdateAttackPowerAndDamage();
    UpdateAttackPowerAndDamage(true);
    UpdateArmor();
    UpdateMaxHealth();

    for (uint8 i = POWER_MANA; i < MAX_POWERS; ++i)
        UpdateMaxPower(Powers(i));

    UpdateAllResistances();

    if (IsWarlockMinion())
        UpdateSpellPower();

    return true;
}

void Guardian::UpdateResistances(uint32 school)
{
    if (school > SPELL_SCHOOL_NORMAL)
    {
        float baseValue = GetModifierValue(UnitMods(UNIT_MOD_RESISTANCE_START + school), BASE_VALUE);
        float bonusValue = GetTotalAuraModValue(UnitMods(UNIT_MOD_RESISTANCE_START + school)) - baseValue;

        // hunter and warlock pets gain 40% of owner's resistance
        if (IsPet())
        {
            baseValue += float(CalculatePct(m_owner->GetResistance(SpellSchools(school)), 40));
            bonusValue += float(CalculatePct(m_owner->GetBonusResistanceMod(SpellSchools(school)), 40));
        }

        SetResistance(SpellSchools(school), int32(baseValue));
        SetBonusResistanceMod(SpellSchools(school), int32(bonusValue));
    }
    else
        UpdateArmor();
}

void Guardian::UpdateArmor()
{
    float baseValue = 0.0f;
    UnitMods unitMod = UNIT_MOD_ARMOR;

    float armor = GetArmor();
    float value = 0.0f;
    float pctFromOwnerArmor = 0.0f;

    PetType petType = IsHunterPet() ? HUNTER_PET : SUMMON_PET;

    switch (petType)
    {
        case SUMMON_PET:
            switch (GetEntry())
            {
                // Mage
                case ENTRY_WATER_ELEMENTAL:
                    pctFromOwnerArmor = 300.f;
                    break;
                // Warlock
                case ENTRY_FELGUARD:
                case ENTRY_VOIDWALKER:
                    pctFromOwnerArmor = 400.f;
                    break;
                case ENTRY_FELHUNTER:
                case ENTRY_IMP:
                case ENTRY_SUCCUBUS:
                    pctFromOwnerArmor = 300.f;
                    break;
                case ENTRY_NIUZAO:
                    pctFromOwnerArmor = 400.f;
                case ENTRY_XUEN:
                    pctFromOwnerArmor = 100.f;
                default:
                    break;
            }
            break;
        case HUNTER_PET:
            pctFromOwnerArmor = 170.f;
            break;
        default:
            break;
    }

    if (pctFromOwnerArmor)
        armor = CalculatePct(m_owner->GetArmor(), pctFromOwnerArmor);
    else
        TC_LOG_ERROR("entities.pet", "Pet (%s, entry %d) has no pctFromOwnerArmor defined. Armor set to 1.",
            GetGUID().ToString().c_str(), GetEntry());

    // Pets do not have static base values
    if (!(GetModifierValue(unitMod, BASE_VALUE) == armor))
        SetModifierValue(unitMod, BASE_VALUE, armor);

    value += GetModifierValue(unitMod, BASE_VALUE);
    value *= GetModifierValue(unitMod, BASE_PCT);
    value += GetModifierValue(unitMod, TOTAL_VALUE);
    value *= GetModifierValue(unitMod, TOTAL_PCT);
    baseValue = value;

    SetArmor(int32(baseValue), int32(value - baseValue));
}

void Guardian::UpdateMaxHealth()
{
    UnitMods unitMod = UNIT_MOD_HEALTH;

    uint64 health = GetMaxHealth();
    float value = 0.0f;
    float pctFromOwnerHealth = 0.0f;

    PetType petType = IsHunterPet() ? HUNTER_PET : SUMMON_PET;

    switch (petType)
    {
        case SUMMON_PET:
        {
            switch (GetEntry())
            {
                case ENTRY_BLOODWORM:
                    pctFromOwnerHealth = 15.f;
                    break;
                case ENTRY_RISEN_SKULKER:
                    pctFromOwnerHealth = 20.f;
                    break;
                case ENTRY_GHOUL:
                case ENTRY_ABOMINATION:
                    pctFromOwnerHealth = 35.f;
                    break;
                case ENTRY_XUEN:
                case ENTRY_NIUZAO:
                case ENTRY_CHI_JI:
                    pctFromOwnerHealth = 100.f;
                    break;
                case ENTRY_FIRE_ELEMENTAL:
                    pctFromOwnerHealth = 75.f;
                    break;
                case ENTRY_FELGUARD:
                case ENTRY_VOIDWALKER:
                case ENTRY_INFERNAL:
                case ENTRY_WATER_ELEMENTAL:
                    pctFromOwnerHealth = 50.f;
                    break;
                case ENTRY_FELHUNTER:
                case ENTRY_SUCCUBUS:
                    pctFromOwnerHealth = 40.f;
                    break;
                case ENTRY_IMP:
                    pctFromOwnerHealth = 30.f;
                    break;
                default:
                    break;
            }
            break;
        }
        case HUNTER_PET:
            pctFromOwnerHealth = 70.f;
            break;
        default:
            break;
    }

    if (pctFromOwnerHealth)
        health = CalculatePct(m_owner->GetMaxHealth(), pctFromOwnerHealth);

    // Pets do not have static base values
    if (!(GetModifierValue(unitMod, BASE_VALUE) == health))
        SetModifierValue(unitMod, BASE_VALUE, health);

    value += GetModifierValue(unitMod, BASE_VALUE);
    value *= GetModifierValue(unitMod, BASE_PCT);
    value += GetModifierValue(unitMod, TOTAL_VALUE);
    value *= GetModifierValue(unitMod, TOTAL_PCT);

    SetCreateHealth((uint32)value); // Blizz is doing it... dont ask why
    SetMaxHealth((uint64)value);
}

void Guardian::UpdateMaxPower(Powers power)
{
    if (GetPowerIndex(power) == MAX_POWERS)
        return;

    UnitMods unitMod = UnitMods(UNIT_MOD_POWER_START + power);

    float value = GetModifierValue(unitMod, BASE_VALUE) + GetCreatePowers(power);
    value *= GetModifierValue(unitMod, BASE_PCT);
    value += GetModifierValue(unitMod, TOTAL_VALUE);
    value *= GetModifierValue(unitMod, TOTAL_PCT);

    SetMaxPower(power, int32(value));
}

void Guardian::UpdateAttackPowerAndDamage(bool ranged)
{
    if (ranged)
        return;

    float val = 0.0f;
    float bonusAP = 0.0f;
    UnitMods unitMod = UNIT_MOD_ATTACK_POWER;

    if (GetEntry() == ENTRY_IMP)                                   // imp's attack power
        val = GetStat(STAT_STRENGTH) - 10.0f;
    else
        val = 2 * GetStat(STAT_STRENGTH) - 20.0f;

    Player* owner = GetOwner() ? GetOwner()->ToPlayer() : nullptr;
    if (owner)
    {
        if (IsHunterPet())                      //hunter pets benefit from owner's attack power
        {
            float mod = 1.0f;                                                 //Hunter contribution modifier
            bonusAP = owner->GetTotalAttackPowerValue(RANGED_ATTACK) * 0.22f * mod;
            SetBonusDamage(int32(owner->GetTotalAttackPowerValue(RANGED_ATTACK) * 0.1287f * mod));
        }
        else if (IsPetGhoul()) //ghouls benefit from deathknight's attack power (may be summon pet or not)
        {
            bonusAP = owner->GetTotalAttackPowerValue(BASE_ATTACK) * 0.22f;
            SetBonusDamage(int32(owner->GetTotalAttackPowerValue(BASE_ATTACK) * 0.1287f));
        }
        else if (IsSpiritWolf()) //wolf benefit from shaman's attack power
        {
            float dmg_multiplier = 0.31f;
            bonusAP = owner->GetTotalAttackPowerValue(BASE_ATTACK) * dmg_multiplier;
            SetBonusDamage(int32(owner->GetTotalAttackPowerValue(BASE_ATTACK) * dmg_multiplier));
        }
        //demons benefit from warlocks shadow or fire damage
        else if (IsPet())
        {
            int32 fire = owner->m_activePlayerData->ModDamageDonePos[SPELL_SCHOOL_FIRE] - owner->m_activePlayerData->ModDamageDoneNeg[SPELL_SCHOOL_FIRE];
            int32 shadow = owner->m_activePlayerData->ModDamageDonePos[SPELL_SCHOOL_SHADOW] - owner->m_activePlayerData->ModDamageDoneNeg[SPELL_SCHOOL_SHADOW];
            int32 maximum  = (fire > shadow) ? fire : shadow;
            if (maximum < 0)
                maximum = 0;
            SetBonusDamage(int32(maximum * 0.15f));
            bonusAP = maximum * 0.57f;
        }
        //water elementals benefit from mage's frost damage
        else if (GetEntry() == ENTRY_WATER_ELEMENTAL)
        {
            int32 frost = owner->m_activePlayerData->ModDamageDonePos[SPELL_SCHOOL_FROST] - owner->m_activePlayerData->ModDamageDoneNeg[SPELL_SCHOOL_FROST];
            if (frost < 0)
                frost = 0;
            SetBonusDamage(int32(frost * 0.4f));
        }
    }

    SetModifierValue(UNIT_MOD_ATTACK_POWER, BASE_VALUE, val + bonusAP);

    //in BASE_VALUE of UNIT_MOD_ATTACK_POWER for creatures we store data of meleeattackpower field in DB
    float base_attPower  = GetModifierValue(unitMod, BASE_VALUE) * GetModifierValue(unitMod, BASE_PCT);
    float attPowerMultiplier = GetModifierValue(unitMod, TOTAL_PCT) - 1.0f;

    SetAttackPower(int32(base_attPower));
    SetAttackPowerMultiplier(attPowerMultiplier);

    //automatically update weapon damage after attack power modification
    UpdateDamagePhysical(BASE_ATTACK);
}

/*void Guardian::UpdateDamagePhysical(WeaponAttackType attType)
{
    if (attType > BASE_ATTACK)
        return;

    float bonusDamage = 0.0f;
    if (Player* playerOwner = m_owner->ToPlayer())
    {
        //force of nature
        if (GetEntry() == ENTRY_TREANT)
        {
            int32 spellDmg = playerOwner->m_activePlayerData->ModDamageDonePos[SPELL_SCHOOL_NATURE] - playerOwner->m_activePlayerData->ModDamageDoneNeg[SPELL_SCHOOL_NATURE];
            if (spellDmg > 0)
                bonusDamage = spellDmg * 0.09f;
        }
        //greater fire elemental
        else if (GetEntry() == ENTRY_FIRE_ELEMENTAL)
        {
            int32 spellDmg = playerOwner->m_activePlayerData->ModDamageDonePos[SPELL_SCHOOL_FIRE] - playerOwner->m_activePlayerData->ModDamageDoneNeg[SPELL_SCHOOL_FIRE];
            if (spellDmg > 0)
                bonusDamage = spellDmg * 0.4f;
        }
    }

    UnitMods unitMod = UNIT_MOD_DAMAGE_MAINHAND;

    float att_speed = float(GetBaseAttackTime(BASE_ATTACK))/1000.0f;

    float base_value  = GetModifierValue(unitMod, BASE_VALUE) + GetTotalAttackPowerValue(attType)/ 3.5f * att_speed  + bonusDamage;
    float base_pct    = GetModifierValue(unitMod, BASE_PCT);
    float total_value = GetModifierValue(unitMod, TOTAL_VALUE);
    float total_pct   = GetModifierValue(unitMod, TOTAL_PCT);

    float weapon_mindamage = GetWeaponDamageRange(BASE_ATTACK, MINDAMAGE);
    float weapon_maxdamage = GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE);

    float mindamage = ((base_value + weapon_mindamage) * base_pct + total_value) * total_pct;
    float maxdamage = ((base_value + weapon_maxdamage) * base_pct + total_value) * total_pct;

    SetUpdateFieldStatValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::MinDamage), mindamage);
    SetUpdateFieldStatValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::MaxDamage), maxdamage);
}*/

void Guardian::UpdateDamagePhysical(WeaponAttackType attType)
{
    float bonusDamage = 0.0f;
    if (Player* playerOwner = m_owner->ToPlayer())
    {
        //force of nature
        if (GetEntry() == ENTRY_TREANT)
        {
            int32 spellDmg = playerOwner->m_activePlayerData->ModDamageDonePos[SPELL_SCHOOL_NATURE] - playerOwner->m_activePlayerData->ModDamageDoneNeg[SPELL_SCHOOL_NATURE];
            if (spellDmg > 0)
                bonusDamage = spellDmg * 0.09f;
        }
        //greater fire elemental
        else if (GetEntry() == ENTRY_FIRE_ELEMENTAL)
        {
            int32 spellDmg = playerOwner->m_activePlayerData->ModDamageDonePos[SPELL_SCHOOL_FIRE] - playerOwner->m_activePlayerData->ModDamageDoneNeg[SPELL_SCHOOL_FIRE];
            if (spellDmg > 0)
                bonusDamage = spellDmg * 0.4f;
        }
    }

    UnitMods unitMod = UNIT_MOD_DAMAGE_MAINHAND;

    float base_value  = GetModifierValue(unitMod, BASE_VALUE) + GetTotalAttackPowerValue(attType)/ 3.5f * 2.f  + bonusDamage;
    float base_pct    = GetModifierValue(unitMod, BASE_PCT);
    float total_value = GetModifierValue(unitMod, TOTAL_VALUE);
    float total_pct   = GetModifierValue(unitMod, TOTAL_PCT);

    float mindamage = (base_value * base_pct + total_value) * total_pct;
    float maxdamage = (base_value * base_pct + total_value) * total_pct;

    AuraEffectList const& mModDamagePercentDone = GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_PERCENT_DONE);
    for (AuraEffectList::const_iterator i = mModDamagePercentDone.begin(); i != mModDamagePercentDone.end(); ++i)
    {
        if ((*i)->GetMiscValue() & SPELL_SCHOOL_MASK_NORMAL)
        {
            AddPct(mindamage, (*i)->GetAmount());
            AddPct(maxdamage, (*i)->GetAmount());
        }
    }

    switch (attType)
    {
        case BASE_ATTACK:
        default:
            SetUpdateFieldStatValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::MinDamage), mindamage);
            SetUpdateFieldStatValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::MaxDamage), maxdamage);
            break;
        case OFF_ATTACK:
            SetUpdateFieldStatValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::MinOffHandDamage), mindamage);
            SetUpdateFieldStatValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::MaxOffHandDamage), maxdamage);
            break;
        case RANGED_ATTACK:
            SetUpdateFieldStatValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::MinRangedDamage), mindamage);
            SetUpdateFieldStatValue(m_values.ModifyValue(&Unit::m_unitData).ModifyValue(&UF::UnitData::MaxRangedDamage), maxdamage);
            break;
    }
}

void Guardian::UpdateSpellPower()
{
    if (HasSameSpellPowerAsOwner())
        SetBonusDamage(m_owner->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_MAGIC));
}

void Guardian::SetBonusDamage(int32 damage)
{
    m_bonusSpellDamage = damage;
    if (Player* playerOwner = GetOwner()->ToPlayer())
        playerOwner->SetPetSpellPower(damage);
}

void Guardian::UpdatePlayerFieldModPetHaste()
{
    if (Player* playerOwner = GetOwner()->ToPlayer())
        playerOwner->SetModPetHaste(m_unitData->ModRangedHaste);
}
