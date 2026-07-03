#include <Arduino.h>
#include <M5Dial.h>
#include <FastLED.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#define NUM_LEDS 29
#define DATA_PIN 13

CRGB leds[NUM_LEDS];

// 描画用のシングルスプライト（SRAM内に1枚だけ確保）
LGFX_Sprite canvas(&M5Dial.Display);

// マルチタスク用の排他制御（ミューテックス）
SemaphoreHandle_t xMutex;

// === コア間で共有するグローバル変数 ===
uint8_t current_mode = 0;
int brightness_val = 128;
int led_offset = 0;
long oldPosition = 0;

// タッチ座標と色情報（初期状態を中央の白に設定）
uint8_t target_hue = 0;
uint8_t target_sat = 0;
int touch_x = 120;
int touch_y = 120;

// 指定された矩形領域の色相環グラデーションをスプライト上で修復する関数
void repairCanvasRegion(int x_start, int y_start, int w, int h) {
    canvas.startWrite();
    for (int y = y_start; y < y_start + h; y++) {
        if (y < 0 || y >= 240) continue;
        for (int x = x_start; x < x_start + w; x++) {
            if (x < 0 || x >= 240) continue;
            float dx = x - 120.0f;
            float dy = y - 120.0f;
            float dist = sqrt(dx * dx + dy * dy);
            if (dist <= 120.0f) {
                float angle = atan2(dy, dx) * 180.0f / PI;
                if (angle < 0) angle += 360.0f;
                
                uint8_t h_val = map(angle, 0, 360, 0, 255);
                uint8_t s_val = map(min(dist, 120.0f), 0, 120, 0, 255);
                
                CRGB c = CHSV(h_val, s_val, 255);
                canvas.writePixel(x, y, canvas.color565(c.r, c.g, c.b));
            } else {
                canvas.writePixel(x, y, TFT_BLACK);
            }
        }
    }
    canvas.endWrite();
}

// スプライト上に英字テキストを描画する関数
void drawTextOnCanvas(uint8_t mode, int bright) {
    canvas.setFont(&fonts::Font4); // 大きめの英数フォント
    canvas.setTextSize(1.2, 1.2);  // さらに大きくスケーリング
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(TFT_BLACK);

    if (mode == 0) {
        int percent = map(bright, 0, 255, 0, 100);
        canvas.drawString("BRIGHTNESS", 120, 105);
        canvas.drawString(String(percent) + " %", 120, 135);
    } else if (mode == 1) {
        canvas.drawString("DIRECTION", 120, 120);
    } else if (mode == 2) {
        canvas.drawString("ALL ON", 120, 120);
    } else if (mode == 3) {
        canvas.drawString("LIVE MODE", 120, 120);
    }
}

// === Core 0 専用タスク（裏側で画面描画だけを専属で行う） ===
void displayTask(void *pvParameters) {
    int last_x = 120;
    int last_y = 120;
    uint8_t last_mode = 255;
    int last_bright = -1;

    while (true) {
        // 1. Core 1から最新の状態を安全に読み取る
        xSemaphoreTake(xMutex, portMAX_DELAY);
        uint8_t l_mode = current_mode;
        int l_bright = brightness_val;
        int l_tx = touch_x;
        int l_ty = touch_y;
        xSemaphoreGive(xMutex);

        bool moved = (l_tx != last_x || l_ty != last_y);
        bool state_changed = (l_mode != last_mode || l_bright != last_bright);

        // 2. 変化があった時だけスプライトを修復・更新して画面へ送る
        if (moved || state_changed) {
            // 古いカーソル位置の背景グラデーションを修復
            if (moved) {
                repairCanvasRegion(last_x - 18, last_y - 18, 36, 36);
            }

            // 文字が変わった、またはカーソルが文字領域を通過した場合は中央を広く修復して文字再描画
            bool near_text = (last_y > 60 && last_y < 180) || (l_ty > 60 && l_ty < 180);
            if (state_changed || near_text) {
                repairCanvasRegion(30, 80, 180, 80);
                drawTextOnCanvas(l_mode, l_bright);
            }

            // 新しい位置にカーソルを描画（間を空けない半径15と16の太い二重の黒線）
            canvas.drawCircle(l_tx, l_ty, 15, TFT_BLACK);
            canvas.drawCircle(l_tx, l_ty, 16, TFT_BLACK);

            // 液晶へ一括転送
            canvas.pushSprite(0, 0);

            // 状態を記憶
            last_x = l_tx;
            last_y = l_ty;
            last_mode = l_mode;
            last_bright = l_bright;
        }

        // Core 0のウォッチドッグ回避と適度なフレームレート維持（約100fps）
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setup() {
    M5Dial.begin(true, false); // RFIDを無効化してGPIO13の競合を回避
    
    // ミューテックス（排他制御）の作成
    xMutex = xSemaphoreCreateMutex();
    
    // FastLEDの初期設定
    FastLED.addLeds<WS2812, DATA_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(brightness_val);
    
    // 初回起動時は白で点灯（モード0の配置）
    FastLED.clear();
    CRGB color = CHSV(target_hue, target_sat, 255);
    int active_leds[4] = {0, 1, 27, 28};
    for (int i = 0; i < 4; i++) {
        leds[active_leds[i]] = color;
    }
    FastLED.show();

    // スプライトの作成と初期描画
    canvas.setColorDepth(16);
    canvas.createSprite(240, 240);
    repairCanvasRegion(0, 0, 240, 240);
    drawTextOnCanvas(current_mode, brightness_val);
    canvas.drawCircle(touch_x, touch_y, 15, TFT_BLACK);
    canvas.drawCircle(touch_x, touch_y, 16, TFT_BLACK);
    canvas.pushSprite(0, 0);
    
    oldPosition = M5Dial.Encoder.read();

    // Core 0 に描画専用タスクを割り当てて起動（スタックサイズ8192、優先度1）
    xTaskCreatePinnedToCore(displayTask, "DisplayTask", 8192, NULL, 1, NULL, 0);
}

// === Core 1 メインループ（入力監視とLED制御だけを行う） ===
void loop() {
    M5Dial.update();

    // Aボタン（ダイヤル押し込み）によるモード切り替え
    if (M5Dial.BtnA.wasPressed()) {
        xSemaphoreTake(xMutex, portMAX_DELAY);
        current_mode = (current_mode + 1) % 4;
        if (current_mode == 1) {
            led_offset = 0;
        }
        xSemaphoreGive(xMutex);
    }

    // タッチパネルによる色選択
    auto t = M5Dial.Touch.getDetail();
    if (t.isPressed()) {
        float dx = t.x - 120.0f;
        float dy = t.y - 120.0f;
        float dist = sqrt(dx * dx + dy * dy);
        float angle = atan2(dy, dx) * 180.0f / PI;
        if (angle < 0) angle += 360.0f;

        xSemaphoreTake(xMutex, portMAX_DELAY);
        touch_x = t.x;
        touch_y = t.y;
        target_hue = map(angle, 0, 360, 0, 255);
        target_sat = map(min(dist, 120.0f), 0, 120, 0, 255);
        xSemaphoreGive(xMutex);
    }

    // ダイヤル回転の読み取り
    long newPosition = M5Dial.Encoder.read();
    long delta = newPosition - oldPosition;
    oldPosition = newPosition;

    if (delta != 0) {
        xSemaphoreTake(xMutex, portMAX_DELAY);
        if (current_mode == 0) {
            brightness_val += delta * 5;
            if (brightness_val > 255) brightness_val = 255;
            if (brightness_val < 0) brightness_val = 0;
            FastLED.setBrightness(brightness_val);
        } else if (current_mode == 1) {
            led_offset += delta;
            while (led_offset < 0) {
                led_offset += NUM_LEDS;
            }
            led_offset %= NUM_LEDS;
        }
        xSemaphoreGive(xMutex);
    }

    // LED用データの安全な取り出し
    xSemaphoreTake(xMutex, portMAX_DELAY);
    uint8_t l_mode = current_mode;
    uint8_t l_hue = target_hue;
    uint8_t l_sat = target_sat;
    int l_offset = led_offset;
    xSemaphoreGive(xMutex);

    // LEDの発光処理（Core 1が専念して実行）
    FastLED.clear();
    CRGB color = CHSV(l_hue, l_sat, 255);

    if (l_mode == 0 || l_mode == 1) {
        int active_leds[4] = {0, 1, 27, 28};
        int current_offset = (l_mode == 1) ? l_offset : 0;
        
        for (int i = 0; i < 4; i++) {
            int idx = (active_leds[i] + current_offset) % NUM_LEDS;
            leds[idx] = color;
        }
    } else if (l_mode == 2) {
        fill_solid(leds, NUM_LEDS, color);
    } else if (l_mode == 3) {
        uint8_t beat = beat8(60);
        fill_rainbow(leds, NUM_LEDS, beat, 255 / NUM_LEDS);
    }
    FastLED.show();

    // Core 1のループ適度なウェイト（入力検知とLED更新を高速で回す）
    delay(5);
}
