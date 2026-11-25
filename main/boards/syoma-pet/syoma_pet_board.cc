#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "pet_state.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "SyomaPetBoard"

class SyomaPetBoard : public WifiBoard {
private:
    // Касание короче этого – не реплика (случайное задевание, дребезг)
    static constexpr int64_t kMinTalkUs = 400 * 1000;

    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    int64_t talk_started_us_ = 0;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));
        ESP_LOGI(TAG, "Install SSD1306 driver");

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                   DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            // Клик из покоя = начало обмена. Пишем в память из главной задачи,
            // не из обработчика кнопки (запись во флеш тормозит таймеры).
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.Schedule([]() { PetState::GetInstance().OnTalk(); });
            }
            app.ToggleChatState();
        });

        touch_button_.OnPressDown([this]() {
            talk_started_us_ = esp_timer_get_time();
            Application::GetInstance().StartListening();
        });

        touch_button_.OnPressUp([this]() {
            int64_t held_us = esp_timer_get_time() - talk_started_us_;
            auto& app = Application::GetInstance();
            app.StopListening();
            if (talk_started_us_ != 0 && held_us >= kMinTalkUs) {
                app.Schedule([]() { PetState::GetInstance().OnTalk(); });
            }
            talk_started_us_ = 0;
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    // Инструменты для агента. Вызываются уже в главной задаче.
    void InitializeTools() {
        static LampController lamp(LAMP_GPIO);
        auto& mcp = McpServer::GetInstance();

        mcp.AddTool("self.pet.get_state",
            "Pet state: mood, energy, bond (0-100), zone, hours since last petting. "
            "Call once at conversation start and act the zone out. "
            "Never invent or recalculate these numbers. clock=unknown means the pet "
            "does not know how much time has passed.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                return PetState::GetInstance().ToJson();
            });

        mcp.AddTool("self.pet.pet",
            "Register one petting reported by the owner. Raises mood and bond, "
            "daily and 10-minute caps are enforced by the firmware.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                PetState::GetInstance().OnPet();
                return true;
            });

        mcp.AddTool("self.pet.game_result",
            "Register a finished game. win=true if the owner won. "
            "Costs energy, raises mood and bond.",
            PropertyList({Property("win", kPropertyTypeBoolean)}),
            [](const PropertyList& props) -> ReturnValue {
                PetState::GetInstance().OnGame(props["win"].value<bool>());
                return true;
            });
    }

public:
    SyomaPetBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        PetState::GetInstance().Load();
        InitializeTools();
    }

    // Состояние питомца едет и в штатном ответе о устройстве:
    // модель часто зовёт его сама, получает цифры бесплатно.
    std::string GetDeviceStatusJson() override {
        std::string base = WifiBoard::GetDeviceStatusJson();
        cJSON* root = cJSON_Parse(base.c_str());
        if (root == nullptr) {
            return base;
        }
        cJSON_AddItemToObject(root, "pet", PetState::GetInstance().ToJson());
        char* str = cJSON_PrintUnformatted(root);
        std::string result = (str != nullptr) ? str : base;
        if (str != nullptr) {
            cJSON_free(str);
        }
        cJSON_Delete(root);
        return result;
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(SyomaPetBoard);