#ifndef PET_STATE_H_
#define PET_STATE_H_

#include <cJSON.h>

class PetState {
public:
    static constexpr int kUnitsPerPoint = 36000;  // внутренние доли пункта
    static PetState& GetInstance() { static PetState s; return s; }
    void Load();
    void Tick();
    void OnPet();
    void OnTalk();
    void OnGame(bool win);
    cJSON* ToJson();
    int mood() const { return mood_ / kUnitsPerPoint; }
    int energy() const { return energy_; }
    int bond() const { return bond_; }
private:
    void Save();
    void RollDays(int today);
    int AddMood(int points);
    int mood_ = 80 * kUnitsPerPoint, energy_ = 100, bond_ = 50;
    int ts_ = 0, day_ = 0;
    int pet_gain_ = 0, talk_gain_ = 0, games_ = 0, ignore_days_ = 0;
    int pw_start_ = 0, pw_gain_ = 0, last_pet_ = 0;
    bool clock_ok_ = false;
};
#endif