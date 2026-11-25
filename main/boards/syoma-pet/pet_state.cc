#include "pet_state.h"
#include "settings.h"
#include <esp_log.h>
#include <algorithm>
#include <ctime>

#define TAG "PetState"

namespace {
constexpr int kU = PetState::kUnitsPerPoint;
constexpr int kAwakePerSec = 30, kNightPerSec = 5;
constexpr int kFloor = 5, kCeil = 100;
constexpr int kPetGain = 3, kPetWinCap = 15, kPetDayCap = 45, kPetWinSec = 600;
constexpr int kTalkGain = 6, kTalkDayCap = 30;
constexpr int kGameWin = 12, kGamePart = 6, kGameEnergy = 15;
constexpr int kMsk = 3 * 3600;
constexpr int kClockShiftSec = 0;          // ЗАПОЛНИТЬ ПО ЗАМЕРУ ЛОГА
constexpr int kMaxGapSec = 30 * 24 * 3600; // защита от скачка часов

int Clamp(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
int NowUtc() { return (int)time(nullptr) - kClockShiftSec; }
int LocalDay(int utc) { return (utc + kMsk) / 86400; }
int LocalHour(int utc) { return ((utc + kMsk) % 86400) / 3600; }
}

void PetState::Save() {
    Settings s("pet", true);
    s.SetInt("mood", mood_);   s.SetInt("energy", energy_);
    s.SetInt("bond", bond_);   s.SetInt("ts", ts_);
    s.SetInt("day", day_);     s.SetInt("petg", pet_gain_);
    s.SetInt("talkg", talk_gain_); s.SetInt("games", games_);
    s.SetInt("ignore", ignore_days_);
    s.SetInt("pws", pw_start_); s.SetInt("pwg", pw_gain_);
    s.SetInt("lastpet", last_pet_);
}

void PetState::Load() {
    Settings s("pet", false);
    mood_ = s.GetInt("mood", mood_); energy_ = s.GetInt("energy", energy_);
    bond_ = s.GetInt("bond", bond_); ts_ = s.GetInt("ts", 0);
    day_ = s.GetInt("day", 0);       pet_gain_ = s.GetInt("petg", 0);
    talk_gain_ = s.GetInt("talkg", 0); games_ = s.GetInt("games", 0);
    ignore_days_ = s.GetInt("ignore", 0);
    pw_start_ = s.GetInt("pws", 0); pw_gain_ = s.GetInt("pwg", 0);
    last_pet_ = s.GetInt("lastpet", 0);
    mood_ = Clamp(mood_, kFloor * kU, kCeil * kU);
    Tick();
    time_t raw = time(nullptr);
    struct tm t {};
    gmtime_r(&raw, &t);
    ESP_LOGI(TAG, "clock(raw) %04d-%02d-%02d %02d:%02d | mood %d energy %d bond %d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
             mood(), energy_, bond_);
}

void PetState::Tick() {
    clock_ok_ = (int)time(nullptr) > 1700000000;
    if (!clock_ok_) return;                 // без сети часов нет, честно стоим
    int now = NowUtc();
    if (ts_ == 0) { ts_ = now; day_ = LocalDay(now); Save(); return; }
    if (now <= ts_) return;
    if (now - ts_ > kMaxGapSec) ts_ = now - kMaxGapSec;
    int t = ts_;
    while (t < now) {                       // участки день/ночь отдельно
        int sod = (t + kMsk) % 86400;
        bool awake = sod >= 8 * 3600 && sod < 23 * 3600;
        int bound = awake ? 23 * 3600 : (sod < 8 * 3600 ? 8 * 3600 : 24 * 3600);
        int step = std::min(bound - sod, now - t);
        mood_ -= step * (awake ? kAwakePerSec : kNightPerSec);
        mood_ = std::max(mood_, kFloor * kU);
        if (!awake && bound == 8 * 3600 && step == bound - sod) energy_ = 100;
        t += step;
    }
    ts_ = now;
    RollDays(LocalDay(now));
}

void PetState::RollDays(int today) {
    if (day_ == 0) { day_ = today; Save(); return; }
    if (today <= day_) return;
    int steps = today - day_;
    if (steps > 60) { bond_ = Clamp(bond_ - (steps - 60) / 7, 0, 100); steps = 60; }
    for (int i = 0; i < steps; ++i) {
        bool care = (i == 0) && (pet_gain_ > 0 || talk_gain_ > 0);
        bool game = (i == 0) && games_ > 0;
        if (game) bond_ += 2;
        else if (care) bond_ += 1;
        else ignore_days_++;
        if (game || care) ignore_days_ = 0;
        if (ignore_days_ >= 7) { bond_ -= 1; ignore_days_ = 0; }
        bond_ = Clamp(bond_, 0, 100);
        pet_gain_ = talk_gain_ = games_ = pw_gain_ = 0;
    }
    day_ = today;
    Save();
}

int PetState::AddMood(int points) {
    int before = mood();
    mood_ = Clamp(mood_ + points * kU, kFloor * kU, kCeil * kU);
    return mood() - before;
}

void PetState::OnPet() {
    Tick();
    if (!clock_ok_) return;
    int now = NowUtc();
    if (now - pw_start_ >= kPetWinSec) { pw_start_ = now; pw_gain_ = 0; }
    int gain = std::min(kPetGain, kPetWinCap - pw_gain_);
    gain = std::min(gain, kPetDayCap - pet_gain_);
    if (gain > 0) {
        AddMood(gain);
        pw_gain_ += gain; pet_gain_ += gain; last_pet_ = now;
    }
    ESP_LOGI(TAG, "pet +%d, mood %d (day %d/%d)", gain, mood(), pet_gain_, kPetDayCap);
    Save();
}

void PetState::OnTalk() {
    Tick();
    if (!clock_ok_) return;
    int gain = std::min(kTalkGain, kTalkDayCap - talk_gain_);
    if (gain > 0) { AddMood(gain); talk_gain_ += gain; }
    Save();
}

void PetState::OnGame(bool win) {
    Tick();
    AddMood(win ? kGameWin : kGamePart);
    energy_ = Clamp(energy_ - kGameEnergy, 0, 100);
    games_++;
    Save();
}

cJSON* PetState::ToJson() {
    Tick();
    int m = mood();
    const char* zone = m >= 80 ? "искрится" : m >= 60 ? "доволен"
                     : m >= 40 ? "скучает" : m >= 20 ? "дуется" : "ушёл в себя";
    int hour = clock_ok_ ? LocalHour(NowUtc()) : -1;
    bool sleepy = energy_ < 20 || (hour >= 5 && hour < 8);
    if (sleepy) zone = "сонный";
    cJSON* j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "mood", m);
    cJSON_AddNumberToObject(j, "energy", energy_);
    cJSON_AddNumberToObject(j, "bond", bond_);
    cJSON_AddStringToObject(j, "zone", zone);
    cJSON_AddBoolToObject(j, "sleepy", sleepy);
    cJSON_AddNumberToObject(j, "games_today", games_);
    if (clock_ok_ && last_pet_ > 0)
        cJSON_AddNumberToObject(j, "hours_since_pet", (NowUtc() - last_pet_) / 3600);
    cJSON_AddStringToObject(j, "clock", clock_ok_ ? "ok" : "unknown");
    return j;
}