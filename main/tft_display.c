// #TFT-1: см. tft_display.h. Стек: esp_lcd (панель + тач, официальные
// управляемые компоненты espressif/esp_lcd_st7796 и espressif/esp_lcd_touch_ft5x06)
// + esp_lvgl_port (официальный порт-слой LVGL — сам поднимает задачу рендера
// и обработки тача, отдельно вызывать lv_timer_handler() не нужно).
//
// ВАЖНО — честно про риски новой периферии без стенда:
//   SPI-панель (esp_lcd_st7796) и I2C-тач (esp_lcd_touch_ft5x06) — официальные
//   компоненты, инициализируются штатными вызовами по документации, риск
//   ошибки низкий (аналогично W5500/BQ25895 — используем API как есть).
//   Реальные места, которые почти наверняка потребуют одной правки на первом
//   живом запуске (это НОРМАЛЬНО для новой периферии, не повод паниковать):
//     - цвета могут быть перепутаны местами (BGR вместо RGB) — см. #TFT-2;
//     - картинка может быть перевёрнута/зеркалena — см. #TFT-3;
//     - тач может быть повёрнут не в ту сторону — см. #TFT-4.
//   Каждое лечится ОДНОЙ строкой в этом файле, помечено соответствующим тегом.

#include "cps_alarm.h"   // #ALM-1
#include "tft_display.h"
#include "board_i2c.h"
#include "board_spi.h"      // #SD-1: общие выводы SPI дисплея и SD-карты
#include "atomspectra.h"
#include "spectrogram.h"   // spectrogram_get_dose_k() — тот же коэффициент дозы, что у веба
#include "monitor.h"       // monitor_copy_since() — тот же секундный ряд CPS и та же сессия, что у веба
#include "spectrogram.h"   // #TFT-28: строки водопада из кольца платы
#include "sd_card.h"       // #TFT-34: каталоги карты для просмотра файлов
#include <dirent.h>
#include <sys/stat.h>
#include "sys_load.h"      // #SYS-1: загрузка ядер ESP32 для шапки
#include "sd_log.h"        // #SD-3: запись сессии мониторинга на SD
#include "boot_config.h"   // boot_config_tz_min() — пояс отображения из веба (#MON-2)
#include "battery.h"       // battery_get_status() — значок батареи, как #batt-chip веба (#TFT-22)

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "esp_netif.h"
#include <string.h>
#include <math.h>
#include <time.h>

static const char *TAG = "tft";

// ---------------- пины (проверьте под свою разводку!) ----------------
// #TFT-6: SPI2_HOST поднимает ЭТОТ модуль (поддержка W5500 вырезана из проекта,
// раньше шину инициализировал w5500_eth_init()). I2C (CTP_SDA/CTP_SCL) —
// по-прежнему общая шина board_i2c.h (там же BQ25895).
// #SD-1: SCLK/MOSI/MISO общие со слотом SD модуля — заданы в board_spi.h.
#define TFT_PIN_SCLK    BOARD_SPI_SCLK
#define TFT_PIN_MOSI    BOARD_SPI_MOSI
#define TFT_PIN_MISO    BOARD_SPI_MISO
#ifndef TFT_PIN_CS
#define TFT_PIN_CS      BOARD_SPI_LCD_CS   // #SD-5: sd_card.c держит его в 1 до нас
#endif
#ifndef TFT_PIN_DC
#define TFT_PIN_DC      5
#endif
// #TFT-10: у модуля lcdwiki 3.5" IPS ST7796 пин LCD_RST ЕСТЬ (4-й в таблице
// интерфейса). Раньше здесь стояло reset_gpio_num = -1 с комментарием «на этом
// модуле его нет» — неверное допущение: без аппаратного сброса панель может
// не выйти в рабочий режим, а SPI-запись при этом идёт без единой ошибки
// (шина односторонняя, подтверждения от панели нет). GPIO14 освободился
// после удаления W5500 (был его RST).
#ifndef TFT_PIN_RST
#define TFT_PIN_RST     14
#endif
#ifndef TOUCH_PIN_RST
#define TOUCH_PIN_RST   7
#endif
#ifndef TOUCH_PIN_INT
#define TOUCH_PIN_INT   15
#endif

#define TFT_H_RES       480   // логическое разрешение ПОСЛЕ поворота в альбомную
#define TFT_V_RES       320   // ориентацию (родное разрешение панели — портретное 320x480)
// #TFT-11: частота записи ограничена САМИМ контроллером, а не проводами.
// Даташит ST7796S, 7.4.3 "4-SPI Serial Data Transfer Interface Characteristics":
//     TSCYCW  Serial clock cycle (Write)  min 66 ns  -> максимум 15.2 МГц
//     TSCYCR  Serial clock cycle (Read)   min 150 ns -> 6.7 МГц (нам не нужно)
// Стоявшие тут 40 МГц (25 нс) — превышение в 2.6 раза, отсюда и артефакты.
// Промежуточная проверка на 20 МГц (50 нс) ничего не дала именно потому, что
// это ВСЁ ЕЩЁ выше предела — эксперимент был поставлен неверно, а не гипотеза
// неверна. 10 МГц (100 нс) дают полуторакратный запас на перемычки.
// Скорость тут не нужна: полный кадр 480*320*2 = 307200 Б уходит за ~0.25 с,
// а экран обновляется раз в секунду (см. vTaskDelay в tft_task).
// #PERF-1: 20 МГц (было 10, #TFT-11). Выводы 11/12/13 — IOMUX-выводы SPI2 (до
// 80 МГц без GPIO-матрицы). Паспорт ST7796 даёт 15.2 МГц на запись, на практике
// модули работают и на 40 МГц; артефакты прошлых сессий были от поворота
// (#TFT-16), не от частоты. Кадр графика по шине — вдвое быстрее.
#define TFT_PCLK_HZ     (20 * 1000 * 1000)

// Буфер отрисовки LVGL, в пикселях (20 строк экрана = 19200 Б) — в PSRAM.
#define TFT_LVGL_BUF_PX  (TFT_H_RES * 20)
// Промежуточный буфер во ВНУТРЕННЕЙ памяти под DMA (4 строки = 3840 Б).
// Внутренняя RAM здесь делится с WiFi/lwIP и USB-хостом и её впритык (#TFT-17),
// поэтому по минимуму: экран обновляется раз в секунду, и лишние SPI-транзакции
// от мелкого буфера ничего не стоят. Было 10 строк (9600 Б).
#define TFT_TRANS_BUF_PX (TFT_H_RES * 4)

#define TOUCH_I2C_ADDR  0x38  // штатный адрес FT5x06/FT6336

// #TFT-12: ступень инициализации — для бисекции «что именно ломает веб».
// Установлено измерением: при 0 веб открывается, при 2 — нет, причём даже с
// физически ОБЕСТОЧЕННЫМ дисплеем. Значит виновато не железо, а код, и ступень
// 1 делит его пополам:
//   0 — не трогаем дисплей вообще (веб работает — проверено);
//   1 — только SPI-шина, панель и селф-тест. Ни LVGL, ни задач, ни буферов.
//       Веб работает  -> виноват LVGL/задачи/буферы (вторая половина);
//       веб не работает -> виновата сама инициализация шины/панели (первая);
//   2 — всё целиком, рабочий режим.
#define TFT_ENABLE      2

// #TFT-8: диагностическая заливка экрана полосами ДО запуска LVGL — отвечает
// сразу на три вопроса, которые иначе неразличимы при чёрном экране:
//   1) горит ли подсветка и жива ли панель вообще (видно хоть что-то?);
//   2) порядок цветов: сверху должна быть КРАСНАЯ полоса. Синяя сверху =
//      перепутаны R и B → поменять rgb_ele_order (#TFT-2);
//   3) ориентация: полосы идут сверху вниз по короткой стороне (альбом 480x320).
// Выключен: панель им проверена (полосы и квадраты легли точно, 2026-09-28),
// а включённый он держит app_main блокирующей паузой TFT_SELFTEST_MS и на это
// время задерживает старт веб-сервера. Включать только для диагностики панели.
#define TFT_SELFTEST    0
#define TFT_PLOT_MIN_MS 900     // #TFT-33: график спектра — на каждый свип (1 с)
#define TFT_PERF        0   // 1 — в лог длительность отрисовки и передачи кадра (#TFT-26)
#define TFT_SELFTEST_MS 3000

// Диапазон каналов на экране — явный запрос пользователя (1..5000), настройка
// будет позже. Веб показывает все 8192.
// #TFT-29: окно каналов экрана (спектр и водопад) — двойной ползунок на вкладке
// Conf, NVS "tft"/"chwin". Было константой 1..5000 (решение 2026-09-28 «настройка
// диапазона будет позже»). Границы включительно, 0..8191, ширина >= 100 каналов.
// Пишет и читает ТОЛЬКО tft_task (ползунок шлёт команду TFT_CMD_CHWIN).
#define CH_WIN_MAX      (SPECTRUM_CHANNELS - 1)
#define CH_WIN_MIN_SPAN 100
static int s_ch_from = 1, s_ch_to = 5000;
#define SPEC_CH_FROM    s_ch_from
#define SPEC_CH_TO      s_ch_to

// ---------------- экраны (#TFT-18 спектр, #TFT-20 монитор и кнопки) ----------------
// Мини-копия страниц веба по утверждённым макетам 480x320 (2026-09-28). Общие
// шапка (CPS, время, всего, доза) и нижняя панель (вкладки + Start/Stop); между
// ними панель «Спектр» (draw() из web/index.html) или «Монитор» (drawChart() из
// web/monitor.html).
#define BAR_Y           284     // нижняя панель 284..319: 36 px — под палец
#define BTN_W           84
#define BTN_H           30
// #TFT-27: справа — Start и «•••»; «•••» меняет вкладки на Stop/Clear/Save и обратно.
#define MORE_W          48
#define MORE_X          (TFT_H_RES - 6 - MORE_W)
#define START_X         (MORE_X - 8 - BTN_W)
#define ACT_X0          6       // Clear / Save / Files на месте вкладок (#TFT-40: без Stop)
#define ACT_W           76      // #TFT-34: четыре кнопки до Start (было три по 84)
#define ACT_STEP        (ACT_W + 6)
// #TFT-34: экран «Files» — поверх панелей, между шапкой и нижней полосой.
#define FILES_Y         45
#define FILES_MAX       60      // строк в списке: новейшие; остальные — только счётчиком
#define TOAST_Y         58
#define STAT_Y          5       // строка статуса шапки (как подписи столбцов)
#define HDR_R_PAD       6       // правый край шапки: статус и IP прижаты к нему (#TFT-23)
#define DTAP_MS         400     // двойной тап: второе касание не позже, мс (#TFT-24)
#define DTAP_PX         40      // и не дальше от первого, px

// Панель «Спектр».
#define PLOT_X          34      // левее — подписи шкалы Y
#define PLOT_Y          52
#define PLOT_W          440
#define PLOT_H          206
#define XLAB_Y          (PLOT_Y + PLOT_H + 3)
#define XUNIT_W         48      // зона подписи оси справа — деления туда не заходят
#define N_YLAB          10
#define N_XLAB          12

// Панель «Монитор».
#define TILE_Y          51
#define TILE_H          56
#define TILE_W          225
#define MCH_X           44      // шире, чем у спектра: подписи вида "412.3" (fmtCps)
#define MCH_Y           116
#define MCH_W           430
#define MCH_H           138
#define MXLAB_Y         (MCH_Y + MCH_H + 3)
// #MON-2: интервал усреднения и окно SMA — из общей сессии платы (monitor.h).
#define MON_VIEW        200     // видимых интервалов — DEF_VIEW веба
#define MON_CAP         21600   // интервалов в памяти: всё кольцо monitor.c (6 ч) при интервале 1 с
#define MON_CHUNK       256     // сэмплов за один monitor_copy_since()
#define MXLAB_W         56      // подпись "HH:MM:SS"
#define N_MYLAB         5
#define N_MXLAB         8

// #MNT-1 (запрос 2026-10-13): вид «на трубе» — прибор крепят к трубе спектрометра,
// экран ПОРТРЕТНЫЙ 320x480 (поворот LVGL на 90°: порт сам перестраивает MADCTL,
// LVGL сам поворачивает точки тача), всё минимально: крупно CPS, ниже график
// мониторинга, ниже спектр, без вкладок и кнопок. Включается и выключается долгим
// нажатием в центре экрана; режим хранится в NVS "tft"/"mount". Сначала был
// поворот на 180° в альбомной ориентации — пользователь выбрал портрет.
// MNT_ROT: верх портрета — левый край альбомного вида (прибор повёрнут по часовой).
// Если нужен обратный — LV_DISP_ROT_270.
#define MNT_ROT         LV_DISP_ROT_90
#define MNT_PW          TFT_V_RES   // 320
#define MNT_PH          TFT_H_RES   // 480
#define MNT_LP_MS       1000    // долгое нажатие, мс
#define MNT_LP_DX       100     // зона центра: |x-240| <= DX, |y-160| <= DY
#define MNT_LP_DY       70
#define MNT_LP_MOVE     30      // палец сдвинулся дальше — это не долгое нажатие
#define MNT_X           4
#define MNT_W           (MNT_PW - 2 * MNT_X)
#define MNT_MON_Y       84      // #MNT-3: над графиком — полоса CPS (0..MNT_TOP_H)
#define MNT_MON_H       184
#define MNT_TOP_H       (MNT_MON_Y - 4)
#define MNT_SWIPE_DY    60      // #MNT-3: свайп очистки — почти горизонтальный
#define MNT_SPEC_Y      (MNT_MON_Y + MNT_MON_H + 6)
#define MNT_SPEC_H      (MNT_PH - MNT_SPEC_Y - 4)
#define COL_N           (MNT_W > PLOT_W ? MNT_W : PLOT_W)   // размер s_col

// Палитра веба: :root в web/index.html и цвета внутри draw().
#define COL_BG          0x0a0c0a    // --bg
#define COL_PANEL       0x11150f    // --panel
#define COL_BD          0x20281e    // --bd
#define COL_TXT         0xcfd6cd    // --txt
#define COL_DIM         0x8a9686    // --dim
#define COL_ACCENT      0x34d3c0    // --accent
#define COL_PLOT_BG     0x0c0f0b    // фон графика
#define COL_FRAME       0x283024    // рамка графика
#define COL_TICK_Y      0x5a645a
#define COL_TICK_X      0x6b756a
#define COL_AXIS        0x4a534a
#define COL_TAB_OFF     0x4a534a    // неактивная вкладка (Waterfall)
#define COL_RED         0xf05a50    // .btn.red: rgba(240,90,80,…)
#define COL_RED_TXT     0xff8a82    // .btn.red color
#define COL_SMA         0xff8c1a    // SMA_COL в web/monitor.html
#define COL_DOT_OFF     0x5b655a    // .dot.off
#define COL_CHIP_OK     0xaeb6a8    // .chip.ok
#define COL_WARN        0xf0c45a    // .chip.warn, батарея < 50%
#define COL_ALARM       0xff3030    // #ALM-1: число CPS при тревоге (мигает с обычным)
#define GRID_ALPHA      0.05f       // rgba(255,255,255,.05)
#define MGRID_ALPHA     0.04f       // сетка Y монитора: rgba(255,255,255,.04)
#define FILL_ALPHA      (0x1e / 255.0f)   // accent+"1e"

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;

// Всё крупное — в PSRAM: внутренняя RAM занята WiFi/lwIP впритык (#TFT-17).
static spectrum_data_t *s_snap;     // снимок под SPEC_LOCK — поток USB пишет живой спектр
static float           *s_col;      // CPS на колонку графика спектра, COL_N значений
static lv_color_t      *s_plot_buf; // холст спектра PLOT_W x PLOT_H
static lv_color_t      *s_mon_buf;  // холст монитора MCH_W x MCH_H

// Общие: шапка, вкладки, кнопки, всплывающее сообщение.
static lv_obj_t *s_cps_val, *s_cps_unit, *s_time_val, *s_total_val, *s_dose_cap, *s_dose_val;
// #SYS-1: загрузка процессора спектрометра и ядер ESP32 — место колонки дозы
// (x 308); при заданном коэффициенте дозы её место занимает доза.
static lv_obj_t *s_cpu_cap, *s_cpu_val;
// #TFT-22: статус прибора и батарея — правый край шапки, как чипы веба.
static lv_obj_t *s_dot, *s_dev_lbl, *s_batt, *s_batt_fill, *s_batt_pct, *s_ip;
// #TFT-39: состояние набора показывает сама точка DEV (отдельная «● R» из
// #TFT-37/38 не помещалась в шапке): серая — прибора нет, бирюзовая — подключён,
// набор не идёт, мигает красным — набор идёт. Мигает lv_timer в задаче LVGL.
static bool s_dot_conn, s_dot_acq, s_dot_blink;
#define DOT_BLINK_MS    500
// #TFT-24: шкала Y спектра. По умолчанию линейная, как Lin в вебе; двойной
// тап по графику — Log и обратно. Пишет задача LVGL, читает tft_task.
static volatile bool s_spec_log;
static lv_obj_t *s_tab_btn[4], *s_toast;   // #TFT-29: 4 вкладки; #TFT-36: кнопки
// #TFT-27: нижняя панель в двух режимах (вкладки / действия) и подтверждение Clear.
static lv_obj_t *s_tabs, *s_acts, *s_act_save, *s_confirm, *s_confirm_txt;
static lv_obj_t *s_wifi;   // #TFT-41: экран WiFi (lv_layer_top), NULL — закрыт
static lv_obj_t *s_alm;    // #ALM-1: экран порогов сигнализации (lv_layer_top), NULL — закрыт
// #TFT-29: Start и «•••» скрываются на вкладке Conf; окно подтверждения общее —
// текст и надпись кнопки задаёт вызвавший, команда уходит в tft_task.
static lv_obj_t *s_btn_start, *s_btn_more, *s_confirm_ok_lbl;
// #TFT-37: кнопка Start становится Stop, пока на текущей вкладке что-то идёт
// (набор спектра / сессия мониторинга / запись водопада). Пишет start_btn_set()
// под замком LVGL, читает обработчик касания — тоже под ним.
static bool s_btn_is_stop;
#define ACQ_HOLD_MS     2500    // «набор идёт»: время набора росло не раньше, мс (свип — раз в 1 с)
#define ACQ_HINT_MS     3000    // после своей команды Start/Stop верим ей, пока прибор не ответил
static void acq_hint(bool start);
// #TFT-34: просмотр файлов и сохранённых спектров. Каталог читает tft_task
// (FATFS/littlefs — не из задачи LVGL: её стек в PSRAM, #RAM-1), список —
// lv_table (строки-ячейки вместо объекта на файл: пул LVGL не резиновый, #TFT-30).
typedef struct { char name[56]; uint32_t size, counts, time_s; time_t saved; int idx; } fent_t;
static fent_t *s_fent;                       // PSRAM, FILES_MAX
static int     s_fent_n, s_fent_more;
static volatile int s_files_dir, s_files_sel;   // от обработчиков касаний к tft_task
static lv_obj_t *s_files, *s_files_tbl, *s_files_dirbtn[3];
static spectrum_data_t *s_saved;             // открытый сохранённый спектр, PSRAM
static bool     s_saved_on;                  // график показывает s_saved, а не живой спектр
static bool     s_force_plot;                // перерисовать график в этом проходе (только tft_task)
static lv_obj_t *s_saved_lbl, *s_live_btn;
static uint8_t   s_confirm_cmd;
static lv_obj_t *s_pane_conf, *s_net_lbl, *s_net_btn[2], *s_chw_slider, *s_chw_lbl;
static bool      s_net_target;              // outdoor для TFT_CMD_NETMODE
static volatile int s_chw_pend_from, s_chw_pend_to;   // от ползунка к tft_task
// Панель «Спектр».
static lv_obj_t *s_pane_spec, *s_plot, *s_msg, *s_xunit;
static lv_obj_t *s_ylab[N_YLAB], *s_xlab[N_XLAB];
// Панель «Монитор».
static lv_obj_t *s_pane_mon, *s_mon_plot, *s_mon_msg, *s_mxunit;
static lv_obj_t *s_sess_val, *s_sess_err, *s_last_val, *s_last_err;
static lv_obj_t *s_last_cap, *s_sma_lbl, *s_mon_stat;
static lv_obj_t *s_raw_lbl;   // #MON-6: «CPS Ns» — интервал сессии (было «raw»)
static lv_obj_t *s_mylab[N_MYLAB], *s_mxlab[N_MXLAB];
// #MNT-1: вид «на трубе» — отдельный непрозрачный слой поверх всего экрана.
static volatile bool s_mount;       // пишет tft_task (и init), читают тач и отрисовка
static lv_indev_t   *s_indev;
static lv_color_t   *s_mnt_mon_buf, *s_mnt_spec_buf;   // PSRAM
static lv_obj_t *s_mnt, *s_mnt_cps, *s_mnt_unit, *s_mnt_dose, *s_mnt_dot, *s_mnt_batt;
static lv_obj_t *s_mnt_mon, *s_mnt_mhi, *s_mnt_mlo, *s_mnt_raw, *s_mnt_sma, *s_mnt_mmsg;
static lv_obj_t *s_mnt_spec, *s_mnt_sinfo, *s_mnt_smsg;
LV_FONT_DECLARE(font_cps64)   // #MNT-2: свой Montserrat 64 px, только цифры (font_cps64.c)
LV_FONT_DECLARE(font_cps104)  // #MNT-3: 104 px — CPS на всю высоту полосы до графика
LV_FONT_DECLARE(font_cps80)   // #MNT-4: 80 px — 4 цифры
static int      s_mnt_fi;        // #MNT-4: текущий шрифт числа (индекс в mnt_layout_top)
static uint32_t s_mnt_up_since;  // крупнее можно с этого момента (lv_tick), 0 — нельзя
static lv_point_t s_mnt_sw0;  // #MNT-3: начало касания спектра (свайп — очистка)
static void mount_set(bool on);
static void mount_save(void);

static lv_color_t s_c_bg, s_c_grid, s_c_mgrid, s_c_fill, s_c_fill_grid, s_c_line, s_c_frame;

// Нажатия -> tft_task. Отправка прибору блокирующая (usb_host_cdc_send ждёт
// мьютекс до 500 мс и передачу до 1 с) — в задаче LVGL её делать нельзя:
// замёрзли бы и отрисовка, и тач.
enum { TFT_CMD_START = 1, TFT_CMD_STOP, TFT_CMD_VIEW, TFT_CMD_CAL, TFT_CMD_CAL_DONE, TFT_CMD_SCALE,
       TFT_CMD_CLEAR, TFT_CMD_SAVE,
       TFT_CMD_NETMODE, TFT_CMD_CHWIN,
       TFT_CMD_FILES, TFT_CMD_FILE_OPEN, TFT_CMD_LIVE,
       TFT_CMD_WIFI_SCAN, TFT_CMD_WIFI_CONNECT, TFT_CMD_MOUNT,   // #MNT-1
       TFT_CMD_MON_START,     // #MNT-2
       TFT_CMD_MNT_CLEAR,     // #MNT-3: свайп по спектру — очистка спектра (не по вкладке)
       TFT_CMD_MNT_MON_CLEAR,
       TFT_CMD_ALARM_SAVE };   // #ALM-1: пороги с экрана -> NVS (tft_task)   // #MNT-5: свайп по графику мониторинга — очистка сессии   // #TFT-41   // #TFT-34   // #TFT-29: после подтверждения / отпускания ползунка   // #TFT-27: CLEAR — уже после подтверждения
static bool          s_ui_ok;       // экран поднят полностью (для веба)
static bool          s_lvgl_up;     // #SD-2: порт LVGL запущен — шина SPI занята его кадрами
static QueueHandle_t s_cmd_q;
// #TFT-32: tft_task будит коммит свипа прибора (как monitor/wf_task), а не свой
// таймер: цикл 1 с + отрисовка давали период ~1040 мс против 1000 мс прибора —
// счётчик времени в шапке то повторял секунду, то перескакивал.
static SemaphoreHandle_t s_commit_sig;
static QueueSetHandle_t  s_wake_set;
static volatile int  s_view;        // 0 — Спектр, 1 — Монитор; пишет только задача LVGL
static TickType_t    s_toast_until; // 0 — сообщение не показано

// Мониторинг (#MON-2): интервалы ОБЩЕЙ сессии платы (monitor.h) — той же, что
// видит веб. Логика pushBase()/closeBucket()/syncSeries() из web/monitor.html:
// копим секундные сэмплы, пока Σdur < интервала сессии; сессия сменилась
// (epoch:rev) — перестраиваем с её начала; Стоп — недобранное ведро выбрасываем.
typedef struct { uint32_t dev_start, dev_end, counts, dur, up; } mon_iv_t;   // up — аптайм конца (→ стена)
static mon_iv_t         *s_iv;      // закрытые интервалы, PSRAM
static int               s_iv_n;
static mon_iv_t          s_bucket;  // незакрытый интервал
static bool              s_bucket_open;
static uint64_t          s_sess_counts, s_sess_dur;
static uint32_t          s_sess_t0; // наборное время начала сессии — ноль оси X без часов
static monitor_sample_t *s_mon_tmp; // приёмник monitor_copy_since(), PSRAM
static uint32_t          s_mon_seq; // последний обработанный seq
static monitor_session_t s_ms;      // сессия из последнего снимка
static uint32_t          s_key_epoch, s_key_rev;
static bool              s_have_key;
// Рабочие массивы отрисовки — в PSRAM, внутренняя RAM на счету (#TFT-17).
static float            *s_m_raw, *s_m_sma;
static lv_point_t       *s_m_pr, *s_m_ps;

// #TFT-28: водопад — строки кольца spectrogram.c (те же, что шлёт веб по
// WebSocket, интервал WF_INTERVAL_*). 1 px на строку, новейшая сверху, как
// draw() в web/waterfall.html. Каналы 1..5000 сжимаются в колонки ПО МАКСИМУМУ,
// как спектр (#TFT-18); веб берёт точку, но на 11 каналов в пикселе точка
// выбрасывала бы 90% счёта. Храним уже сжатые строки: PLOT_H x PLOT_W uint16.
#define WF_FETCH_MAX    4       // новых строк за тик без полной перезагрузки окна
#define N_WYLAB         8
static uint16_t   *s_wf_cols;   // кольцо сжатых строк, PSRAM
static uint16_t   *s_wf_rmax;   // максимум строки (для нормировки)
static int         s_wf_n, s_wf_head;   // строк в кольце; куда ляжет следующая
static uint32_t    s_wf_seen;   // total_rows платы, до которого строки взяты
static bool        s_wf_synced; // кольцо экрана соответствует кольцу платы
static float       s_wf_norm = 64;      // normMax веба: плавающий максимум
static uint16_t   *s_wf_rowbuf; // WF_FETCH_MAX строк платы по 16 КБ, PSRAM
static lv_color_t *s_wf_buf;    // холст водопада PLOT_W x PLOT_H, PSRAM
static lv_color_t  s_wf_lut[256];
static wf_status_t s_wf_st;
static lv_obj_t   *s_pane_wf, *s_wf_plot, *s_wf_msg, *s_wf_stat, *s_wf_xunit;
static lv_obj_t   *s_wf_xlab[N_XLAB], *s_wf_ylab[N_WYLAB];
static void wf_build_lut(void);

// Смесь двух цветов 0xRRGGBB с прозрачностью a (как canvas в вебе поверх фона).
static uint32_t mix_hex(uint32_t under, uint32_t over, float a)
{
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        float u = (float)((under >> sh) & 0xff), o = (float)((over >> sh) & 0xff);
        out |= (uint32_t)(u + (o - u) * a + 0.5f) << sh;
    }
    return out;
}

// niceStep() из web/index.html.
static double nice_step(double r, int n)
{
    if (r <= 0) return 1;
    double s = r / n, e = floor(log10(s)), f = s / pow(10, e);
    return (f <= 1.5 ? 1 : f <= 3.5 ? 2 : f <= 7.5 ? 5 : 10) * pow(10, e);
}

// fmtY() из web/index.html в режиме CPS. Вызывается только для целых декад,
// поэтому toExponential(0) веба сводится к "1e<степень>".
static void fmt_y(double v, char *out, size_t n)
{
    if (v >= 1e6)                snprintf(out, n, "%.1fM", v / 1e6);
    else if (v >= 1e4)           snprintf(out, n, "%.0fk", v / 1e3);
    else if (v >= 1e3)           snprintf(out, n, "%.1fk", v / 1e3);
    else if (v > 0 && v < 0.01)  snprintf(out, n, "1e%d", (int)lround(log10(v)));
    else if (v < 1)              snprintf(out, n, "%.3f", v);
    else if (v < 10)             snprintf(out, n, "%.2f", v);
    else if (v < 100)            snprintf(out, n, "%.1f", v);
    else                         snprintf(out, n, "%.0f", v);
}

// ch2kev()/kev2ch() из web/index.html: полином калибровки по номеру канала с 0,
// обратное — бисекцией на [0, 8192], 30 шагов.
static double ch2kev(const spectrum_data_t *sp, double ch)
{
    double e = 0, p = 1;
    for (int i = 0; i <= sp->calib_order && i < CALIB_COEFFS; i++) { e += sp->calibration[i] * p; p *= ch; }
    return e;
}

static double kev2ch(const spectrum_data_t *sp, double kev)
{
    double lo = 0, hi = SPECTRUM_CHANNELS;
    for (int i = 0; i < 30; i++) {
        double m = (lo + hi) / 2;
        if (ch2kev(sp, m) < kev) lo = m; else hi = m;
    }
    return (lo + hi) / 2;
}

// Каналы SPEC_CH_FROM..SPEC_CH_TO -> PLOT_W колонок, CPS = отсчёты / время.
// По МАКСИМУМУ в колонке: в вебе соседние точки ложатся на один пиксель, и
// глаз видит огибающую сверху. Сумма подняла бы шкалу в ~11 раз против веба,
// а выборка через колонку (как было) теряла узкие пики. Возвращает максимум.
static float build_columns(const spectrum_data_t *sp, float tt, int w)   // w <= COL_N
{
    const int range = SPEC_CH_TO - SPEC_CH_FROM + 1;
    float mx = 0;
    int ch0 = SPEC_CH_FROM;
    for (int x = 0; x < w; x++) {
        int ch1 = SPEC_CH_FROM + (int)((int64_t)range * (x + 1) / w);
        if (ch1 <= ch0) ch1 = ch0 + 1;
        uint32_t m = 0;
        for (int ch = ch0; ch < ch1; ch++) if (sp->bins[ch] > m) m = sp->bins[ch];
        s_col[x] = (float)m / tt;
        if (s_col[x] > mx) mx = s_col[x];
        ch0 = ch1;
    }
    return mx;
}

// ---------------- панель ----------------

// #TFT-13: своя последовательность инициализации вместо встроенной в
// espressif/esp_lcd_st7796. Взята из TFT_eSPI (TFT_Drivers/ST7796_Init.h) —
// именно ею этот модуль заводят в Arduino, и lcdwiki в инструкции к модулю
// прямо требует подменить у себя ST7796_Init.h, т.е. дефолтная не годится.
// Отличия от последовательности Espressif, ради которых всё это:
//   * 0xB6 Display Function Control (0x80,0x02,0x3B) — У ESPRESSIF ЕЁ НЕТ
//     ВООБЩЕ. Третий байт задаёт число строк развёртки под 480-строчную
//     панель; без неё режим развёртки остаётся дефолтным.
//   * задержки 120 мс после VCOM и гаммы — у Espressif все задержки нулевые.
// MADCTL (0x36) и COLMOD (0x3A) здесь НЕ задаём: компонент выставляет их сам
// из rgb_ele_order/bits_per_pixel и ругается в лог, если их переопределить.
static const st7796_lcd_init_cmd_t s_st7796_init_lcdwiki[] = {
    {0xf0, (uint8_t []){0xc3}, 1, 0},                    // Command Set control, ext. part I
    {0xf0, (uint8_t []){0x96}, 1, 0},                    // Command Set control, ext. part II
    {0xb4, (uint8_t []){0x01}, 1, 0},                    // Column inversion
    {0xb6, (uint8_t []){0x80, 0x02, 0x3b}, 3, 0},        // Display Function Control (нет у Espressif)
    {0xe8, (uint8_t []){0x40, 0x8a, 0x00, 0x00, 0x29, 0x19, 0xa5, 0x33}, 8, 0},
    {0xc1, (uint8_t []){0x06}, 1, 0},                    // Power control 2
    {0xc2, (uint8_t []){0xa7}, 1, 0},                    // Power control 3
    {0xc5, (uint8_t []){0x18}, 1, 120},                  // VCOM control + пауза 120 мс
    {0xe0, (uint8_t []){0xf0, 0x09, 0x0b, 0x06, 0x04, 0x15, 0x2f, 0x54, 0x42, 0x3c, 0x17, 0x14, 0x18, 0x1b}, 14, 0},
    {0xe1, (uint8_t []){0xe0, 0x09, 0x0b, 0x06, 0x04, 0x03, 0x2b, 0x43, 0x42, 0x3b, 0x16, 0x14, 0x17, 0x1b}, 14, 120},
    {0xf0, (uint8_t []){0x3c}, 1, 0},                    // выключить ext. part I
    {0xf0, (uint8_t []){0x69}, 1, 120},                  // выключить ext. part II + пауза
};

#if TFT_SELFTEST
// Заливает экран четырьмя горизонтальными полосами напрямую через esp_lcd.
// Никакого LVGL — если тут пусто, проблема в подсветке/питании/шине, а не в UI.
static void panel_selftest(void)
{
    uint16_t *line = heap_caps_malloc(TFT_H_RES * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!line) {
        ESP_LOGW(TAG, "#TFT-8: нет памяти под тестовую строку — селф-тест пропущен");
        return;
    }
    // #TFT-19: пишем в панель в обход LVGL, поэтому байты RGB565 меняем сами —
    // ST7796 ждёт старший байт первым. Без этого полосы шли не в тех цветах
    // (красный выходил синим, зелёный красным, синий зелёным).
    #define SWAP16(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))
    static const uint16_t bands[4] = {
        SWAP16(0xF800),  // красный  (R=31,G=0,B=0)
        SWAP16(0x07E0),  // зелёный
        SWAP16(0x001F),  // синий
        SWAP16(0xFFFF),  // белый
    };
    const int band_h = TFT_V_RES / 4;
    for (int b = 0; b < 4; b++) {
        for (int x = 0; x < TFT_H_RES; x++) line[x] = bands[b];
        for (int y = b * band_h; y < (b + 1) * band_h; y++) {
            esp_err_t e = esp_lcd_panel_draw_bitmap(s_panel, 0, y, TFT_H_RES, y + 1, line);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "#TFT-8: draw_bitmap(y=%d) failed: %s", y, esp_err_to_name(e));
                free(line);
                return;
            }
        }
    }
    // #TFT-15: два квадрата ЧАСТИЧНОЙ ширины. Полосы выше пишутся строками во
    // всю ширину — так пишет только селф-тест. LVGL же обновляет куски
    // произвольной ширины, и если адресация окна по X сбоит, полноширинная
    // запись всё равно выглядит правильно, а частичная — нет. Это ровно та
    // разница, которая отличает «чистые полосы» от «мусора при LVGL».
    // Ожидается: ЖЁЛТЫЙ квадрат слева сверху, ГОЛУБОЙ справа снизу, оба
    // квадратные и с чёткими краями.
    const struct { int x, y; uint16_t color; const char *name; } sq[2] = {
        {  40,  40, SWAP16(0xFFE0), "жёлтый слева сверху" },
        { 360, 200, SWAP16(0x07FF), "голубой справа снизу" },
    };
    const int sq_size = 80;
    for (int s = 0; s < 2; s++) {
        for (int x = 0; x < sq_size; x++) line[x] = sq[s].color;
        for (int y = sq[s].y; y < sq[s].y + sq_size; y++) {
            esp_err_t e = esp_lcd_panel_draw_bitmap(s_panel, sq[s].x, y,
                                                    sq[s].x + sq_size, y + 1, line);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "#TFT-15: квадрат %s, draw_bitmap(y=%d): %s",
                         sq[s].name, y, esp_err_to_name(e));
                break;
            }
        }
    }
    free(line);
    ESP_LOGI(TAG, "#TFT-8: селф-тест отрисован БЕЗ ошибок. Ожидается сверху вниз: "
                  "КРАСНЫЙ, зелёный, синий, белый. Пусто -> подсветка/питание. "
                  "Синий сверху вместо красного -> перепутаны R и B (#TFT-2).");
    ESP_LOGI(TAG, "#TFT-15: плюс два квадрата 80x80 частичной ширины: жёлтый в (40,40), "
                  "голубой в (360,200). Смещены/размазаны/не там -> ломается адресация "
                  "окна по X, и это же ломает вывод LVGL.");
    vTaskDelay(pdMS_TO_TICKS(TFT_SELFTEST_MS));
}
#endif

// #TFT-21: калибровка тача, как на КПК: крестики по очереди, по ним — линейное
// преобразование «сырые координаты контроллера -> экран». Замер 2026-09-28
// показал, что флагами swap/mirror тут не обойтись: Y перевёрнут и сжат (~0.4),
// X сжат и смещён (~0.89x+43), у края мёртвая зона. Полное аффинное
// sx = a*rx + b*ry + c,  sy = d*rx + e*ry + f  покрывает перестановку осей,
// отражения, масштаб и смещение сразу. Коэффициенты — в NVS "tft"/"tcal".
// Запуск — кнопкой на странице «Система» веба (POST /api/tft/calibrate).
typedef struct { uint32_t magic; float a, b, c, d, e, f; } touch_cal_t;
#define TCAL_MAGIC          0x31414354u   // "TCA1": сменить при изменении структуры
#define TCAL_N              5
// Допуск невязки — ловить ПРОМАХИ и посторонний контакт, а не придираться к пальцу.
// Замеры 2026-09-28: калибровка на чистом стекле (модуль на столе) — 3.3 px;
// попытки, когда модуль держали рукой или к стеклу прилегала плёнка, — 18-210 px
// (FT6336U отдаёт первым тот контакт, что появился раньше, — палец на крестике
// шёл вторым и отбрасывался). 30 px с запасом отсекает второе.
// Аффинной модели достаточно: на чистых данных невязка 3.3 px.
#define TCAL_MAX_ERR_PX     30
#define TCAL_TIMEOUT_MS     30000         // нет касания столько — отмена, старая остаётся
#define TCAL_MIN_SAMPLES    3             // опросов тача за нажатие — отсекаем дребезг
// С отступом от края: у панели там мёртвая зона (угловые касания не доходили).
static const lv_point_t CAL_PTS[TCAL_N] = { {50, 50}, {430, 50}, {430, 270}, {50, 270}, {240, 160} };

// Всё состояние калибровки трогают только задача LVGL (опрос тача, события) и
// tft_task — обе под lvgl_port_lock, отдельный замок не нужен.
static touch_cal_t s_tcal;               // действует, если magic == TCAL_MAGIC

// #TFT-26: тач опрашивает СВОЯ задача и складывает события в очередь.
// Причина (замер 2026-09-28, TFT_PERF): при наборе спектра экран раз в секунду
// обновляет ~125 тыс. px, и это 435-505 мс. Передача синхронная (esp_lvgl_port
// ждёт каждый кусок trans_size), всё это время задача LVGL не опрашивает тач,
// а FT6336U не хранит историю — касание короче перерисовки терялось целиком.
// Теперь касание не теряется, а доходит до интерфейса после перерисовки.
#define TP_POLL_MS      20
#define TP_QUEUE_LEN    64      // 1.28 с касания при 20 мс — с запасом к перерисовке
// 4 КБ, не 3: при ошибке I2C драйвер тача пишет ESP_LOGE из этой задачи —
// замер 2026-09-28: свободно 1640 Б в норме, 712 Б после одной такой ошибки.
#define TP_TASK_STACK   4096
typedef struct { int16_t x, y, rx, ry; bool pressed, lp; } tp_evt_t;   // rx/ry — сырые (калибровка); lp — #MNT-1
static QueueHandle_t s_tp_q;
static int16_t       s_tp_raw_x, s_tp_raw_y;   // пишет touch_process() в задаче тача
static bool        s_cal_active;
static int         s_cal_idx, s_cal_n;
static int64_t     s_cal_sum_x, s_cal_sum_y;
static float       s_cal_raw[TCAL_N][2];
static TickType_t  s_cal_deadline;
static lv_obj_t   *s_cal_layer, *s_cal_h, *s_cal_v, *s_cal_txt;

// Вызывается драйвером тача (esp_lcd_touch_get_coordinates) с СЫРЫМИ
// координатами — до его отражений и перестановки осей; здесь же и отдаём
// экранные. Флаги swap/mirror в esp_lcd_touch_config_t поэтому нулевые.
static void touch_process(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength,
                          uint8_t *point_num, uint8_t max_point_num)
{
    (void)tp; (void)strength; (void)max_point_num;
    for (int i = 0; i < *point_num; i++) {
        float rx = x[i], ry = y[i], sx, sy;
        if (i == 0) {   // #TFT-26: сырые едут в событии очереди, копит их tp_read_cb
            s_tp_raw_x = (int16_t)x[0];
            s_tp_raw_y = (int16_t)y[0];
        }
        if (s_cal_active) {
            // Идёт калибровка: LVGL отдаём центр экрана — нажатие гарантированно
            // попадёт в оверлей калибровки.
            sx = TFT_H_RES / 2;
            sy = TFT_V_RES / 2;
        } else if (s_tcal.magic == TCAL_MAGIC) {
            sx = s_tcal.a * rx + s_tcal.b * ry + s_tcal.c;
            sy = s_tcal.d * rx + s_tcal.e * ry + s_tcal.f;
        } else {
            sx = ry;   // без калибровки — прежнее отображение: перестановка осей
            sy = rx;
        }
        x[i] = (uint16_t)(sx < 0 ? 0 : sx > TFT_H_RES - 1 ? TFT_H_RES - 1 : sx);
        y[i] = (uint16_t)(sy < 0 ? 0 : sy > TFT_V_RES - 1 ? TFT_V_RES - 1 : sy);
    }
}

// #TFT-26: опрос тача каждые TP_POLL_MS независимо от LVGL. Пока палец на
// стекле — событие на каждый опрос (движение и накопление для калибровки),
// плюс одно событие отпускания.
static void tp_task(void *arg)
{
    (void)arg;
    bool was = false;
    tp_evt_t ev = {0};
    int lost = 0, polls = 0;
    TickType_t lp_t0 = 0;   // #MNT-1: начало нажатия в центре
    int16_t lp_x0 = 0, lp_y0 = 0;
    bool lp_armed = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TP_POLL_MS));
        esp_lcd_touch_point_data_t pt;   // get_data — тоже через touch_process()
        uint8_t cnt = 0;
        esp_lcd_touch_read_data(s_touch);
        bool pr = esp_lcd_touch_get_data(s_touch, &pt, &cnt, 1) == ESP_OK && cnt > 0;
        if (pr) {
            ev.x = (int16_t)pt.x;
            ev.y = (int16_t)pt.y;
            ev.rx = s_tp_raw_x;
            ev.ry = s_tp_raw_y;
        }
        // #MNT-1: долгое нажатие в центре — палец почти не двигался MNT_LP_MS.
        ev.lp = false;
        if (pr && !was) {
            lp_t0 = xTaskGetTickCount();
            lp_x0 = ev.x;
            lp_y0 = ev.y;
            lp_armed = !s_cal_active && LV_ABS(ev.x - TFT_H_RES / 2) <= MNT_LP_DX &&
                       LV_ABS(ev.y - TFT_V_RES / 2) <= MNT_LP_DY;
        }
        if (pr && lp_armed) {
            if (LV_ABS(ev.x - lp_x0) > MNT_LP_MOVE || LV_ABS(ev.y - lp_y0) > MNT_LP_MOVE) {
                lp_armed = false;
            } else if (xTaskGetTickCount() - lp_t0 >= pdMS_TO_TICKS(MNT_LP_MS)) {
                ev.lp = true;   // одно событие на нажатие
                lp_armed = false;
            }
        }
        if (pr || was) {   // отпускание — с координатами последнего касания
            ev.pressed = pr;
            if (xQueueSend(s_tp_q, &ev, 0) != pdTRUE && ++lost % 50 == 1)
                ESP_LOGW(TAG, "очередь тача полна — события теряются (%d)", lost);
        }
        was = pr;
        if (++polls == 500)   // ~10 с работы, стек задачи — во внутренней RAM
            ESP_LOGI(TAG, "tft_touch: стек, минимум свободного %u B из %u",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL), TP_TASK_STACK);
    }
}

// Чтение для LVGL: всё накопленное за время, пока задача LVGL была занята,
// отдаётся подряд (continue_reading) — нажатие и отпускание не теряются.
static void tp_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    static tp_evt_t last;
    tp_evt_t ev;
    if (xQueueReceive(s_tp_q, &ev, 0) == pdTRUE) {
        last = ev;
        if (ev.pressed && s_cal_active) {   // калибровка: сырые за нажатие (задача LVGL)
            s_cal_sum_x += ev.rx;
            s_cal_sum_y += ev.ry;
            s_cal_n++;
        }
        // #MNT-1: долгое нажатие в центре — смена вида. Нажатие у объекта под
        // пальцем снимается (PRESS_LOST, без CLICKED). Не при открытых окнах
        // (подтверждение, WiFi: под пальцем клавиатура) и не при калибровке.
        if (ev.lp && !s_cal_active && !s_wifi && !s_alm && lv_obj_has_flag(s_confirm, LV_OBJ_FLAG_HIDDEN)) {
            lv_indev_wait_release(s_indev);
            uint8_t cmd = TFT_CMD_MOUNT;
            xQueueSend(s_cmd_q, &cmd, 0);
        }
        data->continue_reading = uxQueueMessagesWaiting(s_tp_q) > 0;
    }
    data->point.x = last.x;
    data->point.y = last.y;
    data->state = last.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

#if TFT_PERF
// Вызывается LVGL после каждого обновления экрана: время отрисовки+передачи, пикселей.
static void perf_monitor_cb(lv_disp_drv_t *drv, uint32_t time_ms, uint32_t px)
{
    (void)drv;
    if (time_ms >= 20) ESP_LOGI("tftperf", "refresh %lu ms, %lu px", (unsigned long)time_ms, (unsigned long)px);
}
#endif

static void panel_init(void)
{
    // #TFT-6: max_transfer_sz под размер буфера LVGL, чтобы flush уходил одной
    // транзакцией. Само по себе НЕ обязательно (esp_lcd умеет резать буфер на
    // чанки по spi_bus_get_max_transaction_len(), см. panel_io_spi_tx_color),
    // но лишние транзакции тут ни к чему. Дефолт шины при DMA — 4092 байта.
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = TFT_PIN_MOSI,
        // #SD-1: MISO (вывод 9 модуля, SDO) нужен SD-карте модуля — панель
        // прошивка не читает. Прежний #TFT-14 его не занимал из опасения, что
        // модуль при питании 5 В отдаёт на SDO 5-вольтовую логику; это не было
        // проверено. Модуль питается от 3.3 В (пользователь, 2026-09-28) —
        // вся его логика 3.3 В, вывод безопасен.
        .miso_io_num = TFT_PIN_MISO,
        .sclk_io_num = TFT_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_SPI_MAX_TRANSFER,   // #SD-5: = TFT_LVGL_BUF_PX*2, как у карты
    };
    // #SD-5: обычно шину уже подняла SD-карта (sd_card_init раньше) — «уже
    // поднята» штатно, но драйвер пишет её как ошибку: глушим его тег на вызов.
    esp_log_level_set("spi", ESP_LOG_NONE);
    esp_err_t bus_err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    esp_log_level_set("spi", ESP_LOG_INFO);
    if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "spi_bus_initialize failed: %s — TFT disabled", esp_err_to_name(bus_err));
        return;
    }

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = TFT_PIN_CS,
        .dc_gpio_num = TFT_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = TFT_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_err_t err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_lcd_new_panel_io_spi failed: %s — TFT disabled", esp_err_to_name(err));
        return;
    }

    st7796_vendor_config_t vendor_cfg = {      // #TFT-13
        .init_cmds = s_st7796_init_lcdwiki,
        .init_cmds_size = sizeof(s_st7796_init_lcdwiki) / sizeof(s_st7796_init_lcdwiki[0]),
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .vendor_config = &vendor_cfg,
        .reset_gpio_num = TFT_PIN_RST,   // #TFT-10: LCD_RST подключён (см. выше)
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,   // #TFT-2: если цвета перепутаны — RGB
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7796(io_handle, &panel_config, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_lcd_new_panel_st7796 failed: %s — TFT disabled", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);        // #TFT-2: типично для ST7796; если негатив — false
    // #TFT-16: эти два вызова нужны только для селф-теста, который рисуется до
    // LVGL. Дальше поворот ПЕРЕУСТАНАВЛИВАЕТ lvgl_port_add_disp() из поля
    // .rotation в lvgl_port_display_cfg_t — менять надо ОБА места сразу,
    // иначе LVGL окажется в другой ориентации, чем панель (это давало мусор).
    // #TFT-3: MADCTL = только MV. Было MV|MY — на железе давало зеркало по
    // горизонтали (фото 2026-09-28: текст задом наперёд, шапка сверху).
    // Если станет вверх ногами — mirror(true, true), т.е. MV|MX|MY.
    esp_lcd_panel_swap_xy(s_panel, true);             // портрет 320x480 -> альбом 480x320
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_lcd_panel_disp_on_off(s_panel, true);

#if TFT_SELFTEST
    panel_selftest();   // #TFT-8: до LVGL — «жива ли панель» без лишних слоёв
#endif

    // ---------------- тач ----------------
    i2c_master_bus_handle_t i2c_bus = board_i2c_get_bus();
    if (!i2c_bus) {
        ESP_LOGW(TAG, "shared I2C bus unavailable — touch disabled (экран без тача)");
        return;
    }
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_config.dev_addr = TOUCH_I2C_ADDR;
    err = esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_config, &tp_io_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_lcd_new_panel_io_i2c (touch) failed: %s — touch disabled", esp_err_to_name(err));
        return;
    }

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = TFT_H_RES,
        .y_max = TFT_V_RES,
        .rst_gpio_num = TOUCH_PIN_RST,
        .int_gpio_num = TOUCH_PIN_INT,
        // #TFT-4/#TFT-21: ВСЁ преобразование «сырые -> экран» делает
        // touch_process() (калибровка). Флаги драйвера нулевые: его
        // перестановка и отражения применялись бы поверх уже экранных координат.
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        .process_coordinates = touch_process,
    };
    err = esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &s_touch);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_lcd_touch_new_i2c_ft5x06 failed: %s — touch disabled", esp_err_to_name(err));
        s_touch = NULL;
    }

#if TFT_ENABLE < 2
    // #TFT-12, ступень 1: остановились до LVGL. Шина, панель и селф-тест уже
    // отработали — если веб при этом жив, виновата вторая половина.
    ESP_LOGW(TAG, "#TFT-12: ступень 1 — LVGL и задачи НЕ поднимаем");
    return;
#else
    // ---------------- LVGL порт ----------------
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    // #RAM-1: стек задачи LVGL (7 КБ) — в PSRAM. Внутренней RAM не хватало даже
    // на 512-байтный DMA-буфер сектора SD (замер 2026-09-29, #SD-3). Можно:
    // задача LVGL не пишет во флеш (NVS/littlefs пишет tft_task) — её стек не
    // нужен, пока кэш выключен (CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y).
    lvgl_cfg.task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    lvgl_port_init(&lvgl_cfg);
    s_lvgl_up = true;   // #SD-2: с этого момента кадры идут по шине из задачи LVGL

    // #TFT-7: буфер отрисовки в PSRAM + промежуточный буфер в SRAM (trans_size).
    // История: spi_master считает DMA-пригодной только внутреннюю RAM
    // (esp_ptr_dma_capable -> SOC_DMA_LOW..SOC_DMA_HIGH), поэтому с «голым»
    // буфером в PSRAM он на КАЖДУЮ передачу выделял временный внутренний
    // DMA-буфер под весь чанк (spi_master.c:1173) и падал при нехватке памяти.
    // Я тогда перенёс весь буфер отрисовки в SRAM — ошибка ушла, но 19 КБ
    // внутренней памяти отняты у WiFi/lwIP, и перестал открываться веб при
    // живом пинге (ICMP обрабатывается на месте, TCP требует буферов).
    // Правильный вариант — штатный trans_size: esp_lvgl_port сам держит
    // небольшой SRAM-буфер и гонит через него кадр из PSRAM по частям.
    ESP_LOGI(TAG, "internal DMA heap ДО: %u B (макс. блок %u B); берём в SRAM %u B (trans), в PSRAM %u B (буфер)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)(TFT_TRANS_BUF_PX * sizeof(uint16_t)),
             (unsigned)(TFT_LVGL_BUF_PX * sizeof(uint16_t)));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = s_panel,
        // #TFT-31: LVGL рисует СРАЗУ во внутренний DMA-буфер (4 строки = 3840 Б,
        // тот же объём внутренней RAM, что был у промежуточного буфера #TFT-7).
        // Прежняя схема — буфер 20 строк в PSRAM + поштучная копия каждого
        // пикселя в SRAM — гоняла сотни КБ/с по шине PSRAM, общей с флешем
        // (octal MSPI): код приёма USB читается из флеша через кэш и тормозил.
        // Замер 2026-09-30, 60 с, спектрометр набирает: экран выкл — 4-7 потерь
        // свипов и 10-12 флагов переполнения FTDI; экран вкл — 16 и 81.
        .buffer_size = TFT_TRANS_BUF_PX,
        .double_buffer = false,
        .trans_size = 0,
        // #TFT-16: ПРИЧИНА АРТЕФАКТОВ. Поворот обязан быть задан ЗДЕСЬ, а не
        // только вызовами esp_lcd_panel_swap_xy/mirror в panel_init(): в конце
        // lvgl_port_add_disp() порт зовёт lvgl_port_update_callback(), и тот
        // безусловно переустанавливает поворот панели из ЭТОГО поля
        // (esp_lvgl_port_disp.c: "Apply rotation from initial display
        // configuration"). Поле было нулевым -> порт возвращал панель в
        // портрет 320x480, а LVGL продолжал слать строки по 480 точек: лишние
        // 160 уходили мимо экрана. Полосы селф-теста при этом выглядели
        // правильно, потому что рисуются ДО этого вызова.
        // Значения обязаны совпадать с теми, что в panel_init().
        .rotation = {
            .swap_xy  = true,    // портрет 320x480 -> альбом 480x320
            .mirror_x = false,
            .mirror_y = false,   // #TFT-3: было true — зеркало по горизонтали
        },
        .hres = TFT_H_RES,
        .vres = TFT_V_RES,
        .monochrome = false,
        .flags = { .buff_dma = true, .buff_spiram = false },   // #TFT-31: внутренняя DMA-память
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
#if TFT_PERF
    if (disp) disp->driver->monitor_cb = perf_monitor_cb;
#endif

    // #TFT-26: не lvgl_port_add_touch() — тот читает тач в задаче LVGL, а она
    // во время перерисовки занята до полусекунды. Своя задача + очередь.
    s_tp_q = s_touch ? xQueueCreate(TP_QUEUE_LEN, sizeof(tp_evt_t)) : NULL;
    if (s_tp_q) {
        static lv_indev_drv_t indev_drv;
        lvgl_port_lock(0);
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.disp = disp;
        indev_drv.read_cb = tp_read_cb;
        lv_indev_t *indev = lv_indev_drv_register(&indev_drv);
        s_indev = indev;   // #MNT-1: lv_indev_wait_release() из tp_read_cb
        lvgl_port_unlock();
        // Приоритет выше задачи LVGL (4): опрос не ждёт перерисовку.
        // #RAM-1: стек в PSRAM — задача только I2C и очередь, во флеш не пишет.
        if (!indev || xTaskCreatePinnedToCoreWithCaps(tp_task, "tft_touch", TP_TASK_STACK, NULL, 5, NULL, 1,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
            ESP_LOGW(TAG, "#TFT-26: задача/indev тача не созданы — тач не работает");
    } else if (s_touch) {
        ESP_LOGW(TAG, "#TFT-26: очередь тача не создана — тач не работает");
    }
#endif  // TFT_ENABLE < 2
}

// ---------------- UI ----------------

// ---------------- экран: построение ----------------

static lv_obj_t *mk_rect(lv_obj_t *p, int x, int y, int w, int h, uint32_t hex)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    // Декор касаний не перехватывает — они должны доходить до вкладок и кнопок.
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(hex), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *p, int x, int y, const lv_font_t *font, uint32_t hex, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    return l;
}

// Подпись деления: фиксированная ширина и выравнивание, изначально скрыта.
static lv_obj_t *mk_tick(lv_obj_t *p, int w, lv_text_align_t align, uint32_t hex)
{
    lv_obj_t *l = mk_label(p, 0, 0, &lv_font_montserrat_12, hex, "");
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);   // не переносить на вторую строку
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

// Прозрачная панель над областью 0..BAR_Y; дочерние — в экранных координатах.
static lv_obj_t *mk_pane(lv_obj_t *p)
{
    lv_obj_t *o = mk_rect(p, 0, 0, TFT_H_RES, BAR_Y, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    return o;
}

static void toast_show(const char *txt, uint32_t col);

// Обработчики касаний выполняются в задаче LVGL под её замком: здесь только
// объекты LVGL и очередь — ни прибора, ни данных спектра.
// #TFT-36: кнопки-переключатели (вкладки, папки Files, режим сети) — в стиле
// Start/Stop: выбранная бирюзовая (как Start), прочие серые (как Files и «•••»).
// Под lvgl_port_lock.
static void btn_select(lv_obj_t *b, bool on)
{
    uint32_t col = on ? COL_ACCENT : COL_DIM;
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(b, on ? LV_OPA_20 : LV_OPA_10, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(on ? COL_ACCENT : COL_TXT), 0);
}

// Переключить вкладку. Под lvgl_port_lock (обработчик касания или tft_task).
static void view_set(int v)
{
    if (v == s_view) return;
    s_view = v;
    lv_obj_t *const panes[4] = { s_pane_spec, s_pane_mon, s_pane_wf, s_pane_conf };   // #TFT-28/29
    // На Conf нечего запускать: Start и «•••» прячем.
    if (v == 3) { lv_obj_add_flag(s_btn_start, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_btn_more, LV_OBJ_FLAG_HIDDEN); }
    else        { lv_obj_clear_flag(s_btn_start, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(s_btn_more, LV_OBJ_FLAG_HIDDEN); }
    for (int i = 0; i < 4; i++) {
        if (i == v) lv_obj_clear_flag(panes[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(panes[i], LV_OBJ_FLAG_HIDDEN);
        btn_select(s_tab_btn[i], i == v);
    }
    uint8_t cmd = TFT_CMD_VIEW;                     // перерисовать новую панель сразу
    xQueueSend(s_cmd_q, &cmd, 0);
}

static void tab_cb(lv_event_t *e)
{
    view_set((int)(intptr_t)lv_event_get_user_data(e));
}

// #TFT-34: обработчики экрана «Files» — только очередь и видимость, файлы читает tft_task.
static void files_open_cb(lv_event_t *e)   // кнопка Files и кнопки папок
{
    s_files_dir = (int)(intptr_t)lv_event_get_user_data(e);
    uint8_t cmd = TFT_CMD_FILES;
    xQueueSend(s_cmd_q, &cmd, 0);
}

static void files_close_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_files, LV_OBJ_FLAG_HIDDEN);
}

static void files_tbl_cb(lv_event_t *e)
{
    uint16_t row, col;
    lv_table_get_selected_cell(lv_event_get_target(e), &row, &col);
    if (row == LV_TABLE_CELL_NONE || (int)row >= s_fent_n) return;
    s_files_sel = row;
    uint8_t cmd = TFT_CMD_FILE_OPEN;
    xQueueSend(s_cmd_q, &cmd, 0);
}

static void live_cb(lv_event_t *e)
{
    (void)e;
    uint8_t cmd = TFT_CMD_LIVE;
    xQueueSend(s_cmd_q, &cmd, 0);
}

// В LVGL 8 события двойного клика нет: второй короткий тап не позже DTAP_MS и
// не дальше DTAP_PX. Состояние — своё у каждого объекта (dtap_t).
typedef struct { uint32_t ms; lv_point_t pt; } dtap_t;

static bool dtap_second(dtap_t *d)
{
    lv_point_t pt;
    lv_indev_get_point(lv_indev_get_act(), &pt);
    uint32_t now = lv_tick_get();
    bool second = d->ms && now - d->ms <= DTAP_MS &&
                  LV_ABS(pt.x - d->pt.x) <= DTAP_PX && LV_ABS(pt.y - d->pt.y) <= DTAP_PX;
    if (!second) {
        d->ms = now;
        d->pt = pt;
        return false;
    }
    d->ms = 0;   // третий тап — снова «первый»
    return true;
}

// #MNT-2: двойной тап по графику мониторинга вида «на трубе» — старт сессии
// (кнопок в этом виде нет). Сама команда — в tft_task.
static void mnt_mon_tap_cb(lv_event_t *e)
{
    (void)e;
    static dtap_t d;
    if (!dtap_second(&d)) return;
    uint8_t cmd = TFT_CMD_MON_START;
    xQueueSend(s_cmd_q, &cmd, 0);
}

static void plot_tap_cb(lv_event_t *e);

// #MNT-3 (запрос 2026-10-13): графики вида «на трубе». Свайп справа налево: начало
// в правых 40 % графика, конец в левых 40 %, почти горизонтально. По спектру (от
// больших каналов к нулю) — очистка спектра (как Clear вкладки Spectrum: -rst + сброс
// платы); #MNT-5 — по мониторингу: очистка сессии (как Clear вкладки Monitor).
// Короткий тап на месте — к двойному тапу (спектр: Lin/Log, мониторинг: старт);
// сдвинутый палец тапом не считается. user_data: 0 — спектр, 1 — мониторинг.
static void mnt_swipe_ev_cb(lv_event_t *e)
{
    bool mon = (intptr_t)lv_event_get_user_data(e) != 0;
    lv_event_code_t c = lv_event_get_code(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_get_act(), &pt);
    if (c == LV_EVENT_PRESSED) { s_mnt_sw0 = pt; return; }
    int dx = pt.x - s_mnt_sw0.x, dy = pt.y - s_mnt_sw0.y;
    if (c == LV_EVENT_RELEASED) {
        if (s_mnt_sw0.x >= MNT_X + MNT_W * 6 / 10 && pt.x <= MNT_X + MNT_W * 4 / 10 &&
            LV_ABS(dy) <= MNT_SWIPE_DY) {
            uint8_t cmd = mon ? TFT_CMD_MNT_MON_CLEAR : TFT_CMD_MNT_CLEAR;
            xQueueSend(s_cmd_q, &cmd, 0);
        }
        return;
    }
    if (c == LV_EVENT_SHORT_CLICKED && LV_ABS(dx) <= DTAP_PX && LV_ABS(dy) <= DTAP_PX) {
        if (mon) mnt_mon_tap_cb(e);
        else     plot_tap_cb(e);
    }
}

// #TFT-24: двойной тап по графику спектра — Lin <-> Log.
static void plot_tap_cb(lv_event_t *e)
{
    (void)e;
    static dtap_t d;   // общий для спектра вкладки и вида «на трубе»: виден только один
    if (!dtap_second(&d)) return;
    s_spec_log = !s_spec_log;
    toast_show(s_spec_log ? "Log scale" : "Linear scale", COL_ACCENT);
    uint8_t cmd = TFT_CMD_SCALE;   // перерисовка — в tft_task
    xQueueSend(s_cmd_q, &cmd, 0);
}

static void dot_apply(void);

// #TFT-39: мигание точки DEV при наборе. lv_timer — в задаче LVGL под её замком.
static void dot_blink_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_dot_conn || !s_dot_acq) return;
    s_dot_blink = !s_dot_blink;
    dot_apply();
}

// #TFT-37: одна кнопка — Start или Stop по текущему состоянию вкладки.
static void start_cb(lv_event_t *e)
{
    (void)e;
    uint8_t cmd = s_btn_is_stop ? TFT_CMD_STOP : TFT_CMD_START;
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE)
        ESP_LOGW(TAG, "очередь команд полна — нажатие потеряно");
}

static void btn_cb(lv_event_t *e)
{
    uint8_t cmd = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE)
        ESP_LOGW(TAG, "очередь команд полна — нажатие потеряно");
}

// #TFT-21: крестик текущей точки калибровки и подсказка. Под lvgl_port_lock.
static void cal_show_target(const char *note)
{
    int cx = CAL_PTS[s_cal_idx].x, cy = CAL_PTS[s_cal_idx].y;
    lv_obj_set_pos(s_cal_h, cx - 20, cy - 1);
    lv_obj_set_pos(s_cal_v, cx - 1, cy - 20);
    lv_obj_set_style_bg_color(s_cal_h, lv_color_hex(COL_SMA), 0);
    lv_obj_set_style_bg_color(s_cal_v, lv_color_hex(COL_SMA), 0);
    lv_label_set_text_fmt(s_cal_txt, "%sTouch the centre of the cross  %d / %d",
                          note ? note : "", s_cal_idx + 1, TCAL_N);
    s_cal_n = 0;
    s_cal_sum_x = s_cal_sum_y = 0;
    s_cal_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TCAL_TIMEOUT_MS);
}

// Палец на стекле: крестик бирюзовый — видно, что тач касание почувствовал.
static void cal_press_cb(lv_event_t *e)
{
    (void)e;
    if (!s_cal_active) return;
    lv_obj_set_style_bg_color(s_cal_h, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_color(s_cal_v, lv_color_hex(COL_ACCENT), 0);
}

// Палец отпущен: среднее сырых за нажатие — точка калибровки. Задача LVGL.
static void cal_release_cb(lv_event_t *e)
{
    (void)e;
    if (!s_cal_active) return;
    lv_obj_set_style_bg_color(s_cal_h, lv_color_hex(COL_SMA), 0);
    lv_obj_set_style_bg_color(s_cal_v, lv_color_hex(COL_SMA), 0);
    if (s_cal_n < TCAL_MIN_SAMPLES) {
        // Слишком короткое — отбрасываем, но накопленное ОБНУЛЯЕМ: иначе оно
        // подмешивалось в следующее нажатие.
        ESP_LOGI(TAG, "калибровка тача: точка %d — касание короче %d опросов, пропущено",
                 s_cal_idx + 1, TCAL_MIN_SAMPLES);
        s_cal_n = 0;
        s_cal_sum_x = s_cal_sum_y = 0;
        return;
    }
    s_cal_raw[s_cal_idx][0] = (float)s_cal_sum_x / s_cal_n;
    s_cal_raw[s_cal_idx][1] = (float)s_cal_sum_y / s_cal_n;
    ESP_LOGI(TAG, "калибровка тача: точка %d (%d, %d) -> сырые (%.1f, %.1f), опросов %d",
             s_cal_idx + 1, CAL_PTS[s_cal_idx].x, CAL_PTS[s_cal_idx].y,
             (double)s_cal_raw[s_cal_idx][0], (double)s_cal_raw[s_cal_idx][1], s_cal_n);
    if (++s_cal_idx < TCAL_N) {
        cal_show_target(NULL);
        return;
    }
    s_cal_active = false;   // дальше касания снова идут в интерфейс
    uint8_t cmd = TFT_CMD_CAL_DONE;   // расчёт и запись в NVS — в tft_task, не в задаче LVGL
    xQueueSend(s_cmd_q, &cmd, 0);
}

static lv_obj_t *mk_button(lv_obj_t *p, int x, int y, int w, const char *txt, uint32_t col,
                           uint32_t txt_col, lv_opa_t bg_opa, lv_event_cb_t cb, uint8_t cmd)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, BTN_H);
    lv_obj_set_style_radius(b, 5, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(b, bg_opa, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_PRESSED);   // видно, что нажатие дошло
    lv_obj_set_style_border_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_50, 0);
    // Запас зоны касания: точность тача после калибровки ~20 px (#TFT-21).
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)cmd);
    lv_obj_center(mk_label(b, 0, 0, &lv_font_montserrat_14, txt_col, txt));
    return b;
}

// #TFT-27: «•••» — вкладки <-> действия. Только интерфейс, задача LVGL.
static void more_cb(lv_event_t *e)
{
    (void)e;
    bool to_acts = lv_obj_has_flag(s_acts, LV_OBJ_FLAG_HIDDEN);
    if (to_acts) {
        // Save — только для спектра; на Monitor Clear чистит сессию мониторинга,
        // на Waterfall — водопад (#TFT-28).
        if (s_view != 0) lv_obj_add_flag(s_act_save, LV_OBJ_FLAG_HIDDEN);
        else             lv_obj_clear_flag(s_act_save, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_tabs, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_acts, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_acts, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_tabs, LV_OBJ_FLAG_HIDDEN);
    }
}

// Clear необратим (прибору уходит -rst) — сначала подтверждение, как в вебе.
static void clear_cb(lv_event_t *e)
{
    (void)e;
    lv_label_set_text(s_confirm_txt, s_view == 1 ? "Clear monitoring session?" :
                                     s_view == 2 ? "Clear waterfall?" : "Clear spectrum?");
    lv_label_set_text(s_confirm_ok_lbl, LV_SYMBOL_TRASH "  Clear");
    s_confirm_cmd = TFT_CMD_CLEAR;
    lv_obj_clear_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
}

static void confirm_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
    if (!(intptr_t)lv_event_get_user_data(e)) return;   // Cancel
    uint8_t cmd = s_confirm_cmd;                         // что спросили — то и делаем
    xQueueSend(s_cmd_q, &cmd, 0);
}

// ---------------- WiFi с экрана (#TFT-41, запрос 2026-10-07) ----------------
// Кнопка «WiFi...» на Conf -> список сетей (поиск ~3 с) -> экран пароля с
// клавиатурой LVGL -> подключение. В режиме Indoor плата пробует новую сеть и
// сохраняет её только при успехе (иначе возвращается к прежней); в режимах точки
// доступа — сохраняет, включает Indoor и перезагружается (wifi_manager_try_sta).
// Поиск и подключение блокируют — их делает tft_task; задача LVGL только рисует.
// Объекты создаются при открытии и удаляются при закрытии — пул LVGL (#TFT-30).
#define WNET_MAX 24
static wifi_net_t *s_wnets;                 // PSRAM; пишет tft_task, читает LVGL — под замком
static int s_wnet_n = -2;                   // -2 — поиск идёт, -1 — ошибка
static lv_obj_t *s_wifi_tbl, *s_wifi_stat;                   // экран списка — s_wifi (lv_layer_top)
static lv_obj_t *s_wifi_pw, *s_wifi_ta_ssid, *s_wifi_ta_pass, *s_wifi_kb, *s_wifi_pw_stat, *s_wifi_ssid_lbl;
static char s_try_ssid[WIFI_SSID_MAX + 1], s_try_pass[WIFI_PASS_MAX + 1];
static volatile bool s_wifi_busy;           // идёт подключение
static void get_ip(char *out, size_t n);
static void conf_render(void);

static void ascii_copy(char *dst, size_t cap, const char *src)   // шрифты экрана — только ASCII
{
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = ((unsigned char)src[i] < 0x20 || (unsigned char)src[i] > 0x7E) ? '?' : src[i];
    dst[i] = 0;
}

static void style_ta(lv_obj_t *ta)
{
    lv_obj_set_style_bg_color(ta, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(COL_BD), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(COL_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(ta, lv_color_hex(COL_TXT), 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_ver(ta, 6, 0);
    lv_textarea_set_one_line(ta, true);
}

static void wifi_close(void)
{
    if (!s_wifi) return;
    lv_obj_del_async(s_wifi);   // вместе с экраном пароля; async — зовут из событий его же кнопок
    s_wifi = s_wifi_tbl = s_wifi_stat = NULL;
    s_wifi_pw = s_wifi_ta_ssid = s_wifi_ta_pass = s_wifi_kb = s_wifi_pw_stat = s_wifi_ssid_lbl = NULL;
}

static void wifi_close_cb(lv_event_t *e) { (void)e; wifi_close(); }

static void wifi_scan_cb(lv_event_t *e)
{
    (void)e;
    if (!s_wifi || s_wnet_n == -2) return;
    s_wnet_n = -2;
    lv_label_set_text(s_wifi_stat, "Scanning...");
    uint8_t cmd = TFT_CMD_WIFI_SCAN;
    xQueueSend(s_cmd_q, &cmd, 0);
}

static void wifi_render_list(void)   // под замком LVGL
{
    if (!s_wifi_tbl) return;
    if (s_wnet_n < 0) {
        lv_label_set_text(s_wifi_stat, s_wnet_n == -2 ? "Scanning..." : "Scan failed - press Rescan");
        lv_table_set_row_cnt(s_wifi_tbl, 1);
        lv_table_set_cell_value(s_wifi_tbl, 0, 0, "Other network (hidden SSID)...");
        lv_table_set_cell_value(s_wifi_tbl, 0, 1, "");
        return;
    }
    lv_label_set_text_fmt(s_wifi_stat, "%d networks - tap one to connect", s_wnet_n);
    lv_table_set_row_cnt(s_wifi_tbl, s_wnet_n + 1);
    char a[40], b[24];
    for (int i = 0; i < s_wnet_n; i++) {
        ascii_copy(a, sizeof a, s_wnets[i].ssid);
        snprintf(b, sizeof b, "%d dBm %s", s_wnets[i].rssi, s_wnets[i].secure ? "WPA" : "open");
        lv_table_set_cell_value(s_wifi_tbl, i, 0, a);
        lv_table_set_cell_value(s_wifi_tbl, i, 1, b);
        lv_table_add_cell_ctrl(s_wifi_tbl, i, 0, LV_TABLE_CELL_CTRL_TEXT_CROP);
    }
    lv_table_set_cell_value(s_wifi_tbl, s_wnet_n, 0, "Other network (hidden SSID)...");
    lv_table_set_cell_value(s_wifi_tbl, s_wnet_n, 1, "");
}

static void wifi_pw_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_wifi_busy || !s_wifi_pw) return;
    lv_obj_del_async(s_wifi_pw);   // из событий его кнопок/клавиатуры — только async
    s_wifi_pw = s_wifi_ta_ssid = s_wifi_ta_pass = s_wifi_kb = s_wifi_pw_stat = s_wifi_ssid_lbl = NULL;
}

static void wifi_connect_cb(lv_event_t *e)
{
    (void)e;
    if (s_wifi_busy || !s_wifi_pw) return;
    if (s_wifi_ta_ssid) strlcpy(s_try_ssid, lv_textarea_get_text(s_wifi_ta_ssid), sizeof s_try_ssid);
    if (!s_try_ssid[0]) { lv_label_set_text(s_wifi_pw_stat, "Enter network name"); return; }
    strlcpy(s_try_pass, lv_textarea_get_text(s_wifi_ta_pass), sizeof s_try_pass);
    size_t pl = strlen(s_try_pass);
    if (pl > 0 && pl < 8) { lv_label_set_text(s_wifi_pw_stat, "Password: 8+ characters"); return; }
    s_wifi_busy = true;
    lv_label_set_text(s_wifi_pw_stat, wifi_manager_mode() == NET_MODE_STA ? "Connecting..." : "Saving...");
    lv_obj_set_style_text_color(s_wifi_pw_stat, lv_color_hex(COL_TXT), 0);
    uint8_t cmd = TFT_CMD_WIFI_CONNECT;
    xQueueSend(s_cmd_q, &cmd, 0);
}

static void wifi_ta_focus_cb(lv_event_t *e)
{
    if (s_wifi_kb) lv_keyboard_set_textarea(s_wifi_kb, lv_event_get_target(e));
}

static void wifi_kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) wifi_connect_cb(e);
    else if (c == LV_EVENT_CANCEL) wifi_pw_back_cb(e);
}

// Экран пароля поверх списка. ssid == NULL — скрытая сеть: имя вводится вручную.
static void wifi_open_pw(const char *ssid)
{
    lv_obj_t *p = s_wifi_pw = mk_rect(s_wifi, 0, 0, TFT_H_RES, TFT_V_RES, COL_BG);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    mk_label(p, 12, 16, &lv_font_montserrat_14, COL_DIM, "Network");
    if (ssid) {
        strlcpy(s_try_ssid, ssid, sizeof s_try_ssid);
        char a[40];
        ascii_copy(a, sizeof a, ssid);
        s_wifi_ssid_lbl = mk_label(p, 96, 14, &lv_font_montserrat_16, COL_TXT, a);
        lv_obj_set_width(s_wifi_ssid_lbl, 280);
        lv_label_set_long_mode(s_wifi_ssid_lbl, LV_LABEL_LONG_DOT);
    } else {
        s_try_ssid[0] = 0;
        s_wifi_ta_ssid = lv_textarea_create(p);
        lv_obj_set_pos(s_wifi_ta_ssid, 96, 6);
        lv_obj_set_size(s_wifi_ta_ssid, 280, 36);
        style_ta(s_wifi_ta_ssid);
        lv_textarea_set_max_length(s_wifi_ta_ssid, WIFI_SSID_MAX - 1);
        lv_textarea_set_placeholder_text(s_wifi_ta_ssid, "SSID");
        lv_obj_add_event_cb(s_wifi_ta_ssid, wifi_ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    }
    mk_label(p, 12, 56, &lv_font_montserrat_14, COL_DIM, "Password");
    s_wifi_ta_pass = lv_textarea_create(p);
    lv_obj_set_pos(s_wifi_ta_pass, 96, 46);
    lv_obj_set_size(s_wifi_ta_pass, 280, 36);
    style_ta(s_wifi_ta_pass);
    // Пароль не скрывается (запрос 2026-10-07): видно, что набрано, кнопка Show не нужна.
    lv_textarea_set_max_length(s_wifi_ta_pass, WIFI_PASS_MAX - 1);
    lv_obj_add_event_cb(s_wifi_ta_pass, wifi_ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    // Back — вверху справа от имени сети, Connect — справа от пароля.
    mk_button(p, 386, 9, 84, "Back", COL_DIM, COL_TXT, LV_OPA_10, wifi_pw_back_cb, 0);
    mk_button(p, 386, 49, 84, "Connect", COL_ACCENT, COL_ACCENT, LV_OPA_20, wifi_connect_cb, 0);
    s_wifi_pw_stat = mk_label(p, 12, 88, &lv_font_montserrat_14, COL_DIM,
                              wifi_manager_mode() == NET_MODE_STA ? "Checked before saving"
                                                                  : "Saved, then reboot to Indoor");
    s_wifi_kb = lv_keyboard_create(p);
    // lv_keyboard_create() выравнивает клавиатуру к низу (LV_ALIGN_BOTTOM_MID), и
    // set_pos считался бы от низа — клавиатура уезжала за край экрана (был виден
    // только верхний ряд). Сначала вернуть выравнивание к верхнему левому углу.
    lv_obj_set_align(s_wifi_kb, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(s_wifi_kb, 0, 108);
    lv_obj_set_size(s_wifi_kb, TFT_H_RES, TFT_V_RES - 108);
    lv_obj_set_style_bg_color(s_wifi_kb, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_width(s_wifi_kb, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_kb, 3, 0);
    lv_obj_set_style_pad_gap(s_wifi_kb, 4, 0);
    lv_obj_set_style_bg_color(s_wifi_kb, lv_color_hex(COL_BD), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_wifi_kb, lv_color_hex(COL_ACCENT), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_wifi_kb, lv_color_hex(COL_TXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_wifi_kb, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_wifi_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_wifi_kb, 4, LV_PART_ITEMS);
    lv_obj_add_event_cb(s_wifi_kb, wifi_kb_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *first = s_wifi_ta_ssid ? s_wifi_ta_ssid : s_wifi_ta_pass;
    lv_keyboard_set_textarea(s_wifi_kb, first);
    lv_obj_add_state(first, LV_STATE_FOCUSED);
}

static void wifi_tbl_cb(lv_event_t *e)
{
    uint16_t row, col;
    lv_table_get_selected_cell(lv_event_get_target(e), &row, &col);
    if (row == LV_TABLE_CELL_NONE || s_wifi_pw) return;
    int n = s_wnet_n < 0 ? 0 : s_wnet_n;
    if ((int)row < n) wifi_open_pw(s_wnets[row].ssid);
    else if ((int)row == n) wifi_open_pw(NULL);
}

static void wifi_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_wifi || !s_wnets) return;
    lv_obj_t *w = s_wifi = mk_rect(lv_layer_top(), 0, 0, TFT_H_RES, TFT_V_RES, COL_BG);
    lv_obj_add_flag(w, LV_OBJ_FLAG_CLICKABLE);   // касания не уходят к экрану под ним
    mk_label(w, 12, 12, &lv_font_montserrat_16, COL_TXT, LV_SYMBOL_WIFI "  WiFi networks");
    mk_button(w, 270, 6, 96, LV_SYMBOL_REFRESH " Rescan", COL_DIM, COL_TXT, LV_OPA_10, wifi_scan_cb, 0);
    mk_button(w, 376, 6, 96, LV_SYMBOL_CLOSE " Close", COL_DIM, COL_TXT, LV_OPA_10, wifi_close_cb, 0);
    s_wifi_stat = mk_label(w, 12, 46, &lv_font_montserrat_12, COL_DIM, "");
    s_wifi_tbl = lv_table_create(w);
    lv_obj_set_pos(s_wifi_tbl, 0, 66);
    lv_obj_set_size(s_wifi_tbl, TFT_H_RES, TFT_V_RES - 66);
    lv_table_set_col_cnt(s_wifi_tbl, 2);
    lv_table_set_col_width(s_wifi_tbl, 0, 320);
    lv_table_set_col_width(s_wifi_tbl, 1, TFT_H_RES - 320);
    lv_obj_set_style_bg_color(s_wifi_tbl, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(s_wifi_tbl, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_tbl, 0, 0);
    lv_obj_set_style_radius(s_wifi_tbl, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_tbl, lv_color_hex(COL_PANEL), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_wifi_tbl, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_wifi_tbl, lv_color_hex(COL_BD), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_wifi_tbl, lv_color_hex(COL_TXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_wifi_tbl, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_wifi_tbl, lv_color_hex(COL_BD), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_wifi_tbl, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_side(s_wifi_tbl, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(s_wifi_tbl, 12, LV_PART_ITEMS);   // строки ~44 px — под палец
    lv_obj_add_event_cb(s_wifi_tbl, wifi_tbl_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_wnet_n = -1;   // wifi_scan_cb запустит поиск
    wifi_scan_cb(NULL);
    wifi_render_list();
}

// tft_task: поиск сетей (блокирует ~3 с), затем список — под замком.
static void wifi_scan_run(void)
{
    int n = wifi_manager_scan(s_wnets, WNET_MAX);
    lvgl_port_lock(0);
    s_wnet_n = n < 0 ? -1 : n;
    wifi_render_list();
    lvgl_port_unlock();
}

// tft_task: подключение (блокирует до 20 с), итог — на экране пароля или тостом.
static void wifi_connect_run(void)
{
    char ssid_disp[40];
    ascii_copy(ssid_disp, sizeof ssid_disp, s_try_ssid);
    int r = wifi_manager_try_sta(s_try_ssid, s_try_pass, 20000);
    memset(s_try_pass, 0, sizeof s_try_pass);   // пароль в памяти дольше не нужен
    char msg[80], ip[16] = "";
    uint32_t col = COL_RED_TXT;
    if (r == WIFI_TRY_OK) {
        get_ip(ip, sizeof ip);
        snprintf(msg, sizeof msg, "Connected to %.24s, IP %s", ssid_disp, ip);
        col = COL_ACCENT;
    } else if (r == WIFI_TRY_SAVED_REBOOT) {
        snprintf(msg, sizeof msg, "Saved. Rebooting to Indoor...");
        col = COL_ACCENT;
    } else {
        snprintf(msg, sizeof msg, "%s - previous network kept",
                 r == WIFI_TRY_BAD_PASS ? "Wrong password" : r == WIFI_TRY_NO_AP ? "Network not found" : "Could not connect");
    }
    ESP_LOGI(TAG, "#TFT-41: WiFi %s: %s", ssid_disp, msg);
    lvgl_port_lock(0);
    s_wifi_busy = false;
    if (s_wifi_pw_stat) {
        lv_label_set_text(s_wifi_pw_stat, msg);
        lv_obj_set_style_text_color(s_wifi_pw_stat, lv_color_hex(col), 0);
    } else {
        toast_show(msg, col);
    }
    if (s_view == 3) conf_render();
    lvgl_port_unlock();
    if (r == WIFI_TRY_SAVED_REBOOT) {
        vTaskDelay(pdMS_TO_TICKS(1200));
        esp_restart();
    }
}

// ---------------- #ALM-1: экран порогов сигнализации ----------------
// Conf -> Alarm. Правится копия s_alm_edit (задача LVGL), Save отдаёт её tft_task
// (NVS — не из задачи LVGL: стек в PSRAM, #RAM-1). Как экран WiFi — на lv_layer_top,
// создаётся при открытии, удаляется lv_obj_del_async.
static lv_obj_t *s_alm_val[CPS_ALARM_LEVELS], *s_alm_en_btn;
static cps_alarm_cfg_t s_alm_edit, s_alm_pend;

static void alm_render(void)   // под замком LVGL
{
    for (int i = 0; i < CPS_ALARM_LEVELS; i++)
        lv_label_set_text_fmt(s_alm_val[i], "%lu", (unsigned long)s_alm_edit.th[i]);
    lv_label_set_text(lv_obj_get_child(s_alm_en_btn, 0), s_alm_edit.enabled ? "Alarm ON" : "Alarm OFF");
    btn_select(s_alm_en_btn, s_alm_edit.enabled);
}

static uint32_t alm_step(uint32_t v)
{
    return v < 20 ? 1 : v < 200 ? 5 : v < 1000 ? 25 : v < 5000 ? 100 : 500;
}

// -/+ порога; удержание кнопки — повтор (LONG_PRESSED_REPEAT). Порог остаётся
// строго между соседними. user_data: (уровень << 1) | плюс.
static void alm_pm_cb(lv_event_t *e)
{
    int ud = (int)(intptr_t)lv_event_get_user_data(e), i = ud >> 1;
    uint32_t v = s_alm_edit.th[i];
    uint32_t lo = i ? s_alm_edit.th[i - 1] + 1 : 1;
    uint32_t hi = i < CPS_ALARM_LEVELS - 1 ? s_alm_edit.th[i + 1] - 1 : CPS_ALARM_TH_MAX;
    if (ud & 1) {
        v += alm_step(v);
    } else {
        uint32_t st = alm_step(v > 1 ? v - 1 : 1);
        v = v > st ? v - st : 1;
    }
    s_alm_edit.th[i] = v < lo ? lo : v > hi ? hi : v;
    alm_render();
}

static void alm_close(void)
{
    if (!s_alm) return;
    lv_obj_del_async(s_alm);   // из событий его же кнопок — только async
    s_alm = NULL;
}

static void alm_en_cb(lv_event_t *e)     { (void)e; s_alm_edit.enabled = !s_alm_edit.enabled; alm_render(); }
static void alm_cancel_cb(lv_event_t *e) { (void)e; alm_close(); }

static void alm_save_cb(lv_event_t *e)
{
    (void)e;
    s_alm_pend = s_alm_edit;
    uint8_t cmd = TFT_CMD_ALARM_SAVE;
    xQueueSend(s_cmd_q, &cmd, 0);
    alm_close();
}

static void alm_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_alm) return;
    cps_alarm_get_config(&s_alm_edit);
    static const char *const PAT[CPS_ALARM_LEVELS] = { "1   1 beep", "2   2 beeps", "3   3 beeps", "4   fast" };
    lv_obj_t *w = s_alm = mk_rect(lv_layer_top(), 0, 0, TFT_H_RES, TFT_V_RES, COL_BG);
    lv_obj_add_flag(w, LV_OBJ_FLAG_CLICKABLE);   // касания не уходят к экрану под ним
    mk_label(w, 12, 12, &lv_font_montserrat_16, COL_TXT, LV_SYMBOL_BELL "  CPS alarm");
    mk_button(w, 296, 6, 84, "Cancel", COL_DIM, COL_TXT, LV_OPA_10, alm_cancel_cb, 0);
    mk_button(w, 386, 6, 84, LV_SYMBOL_OK " Save", COL_ACCENT, COL_ACCENT, LV_OPA_20, alm_save_cb, 0);
    s_alm_en_btn = mk_button(w, 12, 46, 150, "", COL_DIM, COL_TXT, LV_OPA_10, alm_en_cb, 0);
    mk_label(w, 176, 54, &lv_font_montserrat_12, COL_DIM, "CPS per 1 s, speaker GPIO21");
    for (int i = 0; i < CPS_ALARM_LEVELS; i++) {
        int y = 94 + i * 54;
        mk_label(w, 12, y + 6, &lv_font_montserrat_16, COL_TXT, PAT[i]);
        lv_obj_t *m = mk_button(w, 150, y, 70, LV_SYMBOL_MINUS, COL_DIM, COL_TXT, LV_OPA_10, alm_pm_cb, (uint8_t)(i << 1));
        lv_obj_t *p = mk_button(w, 400, y, 70, LV_SYMBOL_PLUS, COL_DIM, COL_TXT, LV_OPA_10, alm_pm_cb, (uint8_t)(i << 1 | 1));
        lv_obj_add_event_cb(m, alm_pm_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)(i << 1));
        lv_obj_add_event_cb(p, alm_pm_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)(i << 1 | 1));
        s_alm_val[i] = mk_label(w, 230, y - 1, &lv_font_montserrat_28, COL_ACCENT, "");
        lv_obj_set_width(s_alm_val[i], 160);
        lv_obj_set_style_text_align(s_alm_val[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    alm_render();
}

// #ALM-1: число CPS (шапка и вид «на трубе») при тревоге мигает красным раз в
// секунду (500 мс красное / 500 мс обычное). lv_timer — задача LVGL.
static void alm_blink_cb(lv_timer_t *t)
{
    (void)t;
    static bool phase;
    static uint32_t last = COL_ACCENT;
    phase = cps_alarm_level() > 0 ? !phase : false;
    uint32_t col = phase ? COL_ALARM : COL_ACCENT;
    if (col == last) return;
    last = col;
    lv_obj_set_style_text_color(s_cps_val, lv_color_hex(col), 0);
    if (s_mnt_cps) lv_obj_set_style_text_color(s_mnt_cps, lv_color_hex(col), 0);
}

// #TFT-29: режим сети. Переключение — перезагрузка платы, поэтому через подтверждение.
static void net_btn_cb(lv_event_t *e)
{
    bool outdoor = (intptr_t)lv_event_get_user_data(e) != 0;
    bool now_out = wifi_manager_mode() == NET_MODE_FIELD_AP;
    if (outdoor == now_out && wifi_manager_mode() != NET_MODE_SETUP) {
        toast_show("Already in this mode", COL_DIM);
        return;
    }
    s_net_target = outdoor;
    lv_label_set_text(s_confirm_txt, outdoor ? "Switch to Field mode (Outdoor AP)?\nThe board will reboot."
                                             : "Switch to Home network (Indoor)?\nThe board will reboot.");
    lv_label_set_text(s_confirm_ok_lbl, "Switch");
    s_confirm_cmd = TFT_CMD_NETMODE;
    lv_obj_clear_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
}

// #TFT-29: двойной ползунок окна каналов. Во время движения держим ширину
// >= CH_WIN_MIN_SPAN (упирается двигаемая ручка), на отпускании — применить.
static void chw_slider_cb(lv_event_t *e)
{
    static int last_l = -1;
    lv_obj_t *sl = lv_event_get_target(e);
    int l = lv_slider_get_left_value(sl), r = lv_slider_get_value(sl);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        if (r - l < CH_WIN_MIN_SPAN) {
            if (l != last_l) {   // двигали левую
                l = r - CH_WIN_MIN_SPAN;
                if (l < 0) { l = 0; r = CH_WIN_MIN_SPAN; }
            } else {
                r = l + CH_WIN_MIN_SPAN;
                if (r > CH_WIN_MAX) { r = CH_WIN_MAX; l = r - CH_WIN_MIN_SPAN; }
            }
            lv_slider_set_value(sl, r, LV_ANIM_OFF);
            lv_slider_set_left_value(sl, l, LV_ANIM_OFF);
        }
        last_l = l;
        lv_label_set_text_fmt(s_chw_lbl, "%d - %d", l, r);
    } else {   // LV_EVENT_RELEASED
        s_chw_pend_from = l;
        s_chw_pend_to = r;
        uint8_t cmd = TFT_CMD_CHWIN;
        xQueueSend(s_cmd_q, &cmd, 0);
    }
}

// Плитка монитора: подпись, крупное CPS, погрешность справа внизу.
static lv_obj_t *mk_tile(lv_obj_t *p, int x, const char *cap, lv_obj_t **val, lv_obj_t **err)
{
    lv_obj_t *t = mk_rect(p, x, TILE_Y, TILE_W, TILE_H, COL_PANEL);
    lv_obj_set_style_border_color(t, lv_color_hex(COL_BD), 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_radius(t, 4, 0);
    lv_obj_t *c = mk_label(p, x + 10, TILE_Y + 5, &lv_font_montserrat_12, COL_DIM, cap);
    *val = mk_label(p, x + 10, TILE_Y + 21, &lv_font_montserrat_28, COL_ACCENT, "--");
    *err = mk_label(p, x + TILE_W - 110, TILE_Y + 33, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_set_width(*err, 100);
    lv_obj_set_style_text_align(*err, LV_TEXT_ALIGN_RIGHT, 0);
    return c;
}

static bool build_ui(void)
{
    s_snap     = heap_caps_malloc(sizeof(*s_snap), MALLOC_CAP_SPIRAM);
    s_col      = heap_caps_malloc(COL_N * sizeof(float), MALLOC_CAP_SPIRAM);
    s_plot_buf = heap_caps_malloc((size_t)PLOT_W * PLOT_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    s_mon_buf  = heap_caps_malloc((size_t)MCH_W * MCH_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    s_iv       = heap_caps_malloc(MON_CAP * sizeof(*s_iv), MALLOC_CAP_SPIRAM);
    s_mon_tmp  = heap_caps_malloc(MON_CHUNK * sizeof(*s_mon_tmp), MALLOC_CAP_SPIRAM);
    s_m_raw    = heap_caps_malloc(MON_VIEW * sizeof(float), MALLOC_CAP_SPIRAM);
    s_m_sma    = heap_caps_malloc(MON_VIEW * sizeof(float), MALLOC_CAP_SPIRAM);
    s_m_pr     = heap_caps_malloc(MON_VIEW * sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
    s_m_ps     = heap_caps_malloc(MON_VIEW * sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
    s_wf_cols  = heap_caps_calloc((size_t)PLOT_H * PLOT_W, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_wf_rmax  = heap_caps_calloc(PLOT_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_wf_rowbuf = heap_caps_malloc((size_t)WF_FETCH_MAX * WF_ROW_BYTES, MALLOC_CAP_SPIRAM);
    s_wf_buf   = heap_caps_malloc((size_t)PLOT_W * PLOT_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    s_fent     = heap_caps_calloc(FILES_MAX, sizeof(*s_fent), MALLOC_CAP_SPIRAM);   // #TFT-34
    s_wnets    = heap_caps_calloc(WNET_MAX, sizeof(*s_wnets), MALLOC_CAP_SPIRAM);  // #TFT-41
    s_saved    = heap_caps_malloc(sizeof(*s_saved), MALLOC_CAP_SPIRAM);
    s_mnt_mon_buf  = heap_caps_malloc((size_t)MNT_W * MNT_MON_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);   // #MNT-1
    s_mnt_spec_buf = heap_caps_malloc((size_t)MNT_W * MNT_SPEC_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!s_snap || !s_col || !s_plot_buf || !s_mon_buf || !s_iv || !s_mon_tmp ||
        !s_m_raw || !s_m_sma || !s_m_pr || !s_m_ps ||
        !s_wf_cols || !s_wf_rmax || !s_wf_rowbuf || !s_wf_buf || !s_fent || !s_saved ||
        !s_mnt_mon_buf || !s_mnt_spec_buf) {
        ESP_LOGE(TAG, "malloc failed for TFT buffers — TFT UI disabled");
        return false;
    }
    uint32_t grid_hex = mix_hex(COL_PLOT_BG, 0xffffff, GRID_ALPHA);
    s_c_bg        = lv_color_hex(COL_PLOT_BG);
    s_c_grid      = lv_color_hex(grid_hex);
    s_c_mgrid     = lv_color_hex(mix_hex(COL_PLOT_BG, 0xffffff, MGRID_ALPHA));
    s_c_fill      = lv_color_hex(mix_hex(COL_PLOT_BG, COL_ACCENT, FILL_ALPHA));
    s_c_fill_grid = lv_color_hex(mix_hex(grid_hex, COL_ACCENT, FILL_ALPHA));
    s_c_line      = lv_color_hex(COL_ACCENT);
    s_c_frame     = lv_color_hex(COL_FRAME);
    // #TFT-35: буферы canvas из malloc — в них мусор PSRAM, и до первой отрисовки
    // (спектр ждёт данных, монитор — интервала, водопад — открытия вкладки) экран
    // показывал его «шумом» при загрузке. Сразу заливаем фоном графика.
    for (int i = 0; i < PLOT_W * PLOT_H; i++) s_plot_buf[i] = s_wf_buf[i] = s_c_bg;
    for (int i = 0; i < MCH_W * MCH_H; i++) s_mon_buf[i] = s_c_bg;
    for (int i = 0; i < MNT_W * MNT_MON_H; i++) s_mnt_mon_buf[i] = s_c_bg;
    for (int i = 0; i < MNT_W * MNT_SPEC_H; i++) s_mnt_spec_buf[i] = s_c_bg;

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Шапка — общая, те же величины, что крупно в шапке web/index.html.
    mk_rect(scr, 0, 0, TFT_H_RES, 44, COL_PANEL);
    mk_rect(scr, 0, 44, TFT_H_RES, 1, COL_BD);
    s_cps_val  = mk_label(scr, 10, 6, &lv_font_montserrat_28, COL_ACCENT, "--");
    s_cps_unit = mk_label(scr, 0, 0, &lv_font_montserrat_12, COL_DIM, "CPS");
    mk_label(scr, 142, 5, &lv_font_montserrat_12, COL_DIM, "time");
    s_time_val  = mk_label(scr, 142, 20, &lv_font_montserrat_16, COL_TXT, "--:--:--");
    mk_label(scr, 220, 5, &lv_font_montserrat_12, COL_DIM, "total");
    s_total_val = mk_label(scr, 220, 20, &lv_font_montserrat_16, COL_TXT, "--");
    // «µ» и «±» в шрифте нет: диапазоны генератора 0x20-0x7F,0xB0,0x2022.
    s_dose_cap  = mk_label(scr, 308, 5, &lv_font_montserrat_12, COL_DIM, "uSv/h");
    s_dose_val  = mk_label(scr, 308, 20, &lv_font_montserrat_16, COL_TXT, "--");
    lv_obj_add_flag(s_dose_cap, LV_OBJ_FLAG_HIDDEN);   // покажет render_header, если задан коэффициент
    lv_obj_add_flag(s_dose_val, LV_OBJ_FLAG_HIDDEN);
    s_cpu_cap   = mk_label(scr, 308, 5, &lv_font_montserrat_12, COL_DIM, "D|1|2");   // прибор | ядро 0 | ядро 1, %
    s_cpu_val   = mk_label(scr, 308, 21, &lv_font_montserrat_14, COL_TXT, "--");
    // #TFT-22/#TFT-23: чипы шапки веба одной строкой у правого края — прибор
    // (#dev-chip: r.usb_connected) и батарея (#batt-chip: только если BQ25895
    // найден на I2C); под ними IP платы. #TFT-25: позиции считает
    // layout_status() справа налево по реальной ширине текста. Flex-контейнер
    // с LV_SIZE_CONTENT здесь был: на железе он обрезал строку слева — пропадали
    // статус прибора и край батареи.
    s_dot = mk_rect(scr, 0, STAT_Y + 5, 7, 7, COL_DOT_OFF);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    s_dev_lbl = mk_label(scr, 0, STAT_Y, &lv_font_montserrat_12, COL_WARN, "--");
    s_batt = mk_rect(scr, 0, STAT_Y + 3, 18, 10, 0);   // корпус: рамка 1.4px в вебе -> 1px
    lv_obj_set_style_bg_opa(s_batt, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_batt, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_border_width(s_batt, 1, 0);
    lv_obj_set_style_radius(s_batt, 2, 0);
    lv_obj_set_style_pad_all(s_batt, 1, 0);
    s_batt_fill = mk_rect(s_batt, 0, 0, 0, 6, COL_ACCENT);
    // «Плюс» справа снаружи корпуса. Координаты детей — от области содержимого
    // (рамка 1 + отступ 1), поэтому x 16 = сразу за правым краем корпуса.
    mk_rect(s_batt, 16, 1, 2, 4, COL_DIM);
    lv_obj_add_flag(s_batt, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    s_batt_pct = mk_label(scr, 0, STAT_Y, &lv_font_montserrat_12, COL_CHIP_OK, "");
    lv_obj_add_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_batt_pct, LV_OBJ_FLAG_HIDDEN);
    s_ip = mk_label(scr, 0, 0, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_align(s_ip, LV_ALIGN_TOP_RIGHT, -HDR_R_PAD, 22);
    lv_timer_create(dot_blink_cb, DOT_BLINK_MS, NULL);   // #TFT-39
    lv_timer_create(alm_blink_cb, 500, NULL);            // #ALM-1: мигание CPS при тревоге

    // Панель «Спектр».
    s_pane_spec = mk_pane(scr);
    s_plot = lv_canvas_create(s_pane_spec);
    lv_canvas_set_buffer(s_plot, s_plot_buf, PLOT_W, PLOT_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(s_plot, PLOT_X, PLOT_Y);
    lv_obj_add_flag(s_plot, LV_OBJ_FLAG_CLICKABLE);   // #TFT-24: двойной тап — Lin/Log
    lv_obj_add_event_cb(s_plot, plot_tap_cb, LV_EVENT_SHORT_CLICKED, NULL);
    for (int i = 0; i < N_YLAB; i++) s_ylab[i] = mk_tick(s_pane_spec, PLOT_X - 1, LV_TEXT_ALIGN_RIGHT, COL_TICK_Y);
    for (int i = 0; i < N_XLAB; i++) s_xlab[i] = mk_tick(s_pane_spec, 40, LV_TEXT_ALIGN_CENTER, COL_TICK_X);
    s_xunit = mk_tick(s_pane_spec, XUNIT_W, LV_TEXT_ALIGN_RIGHT, COL_AXIS);
    lv_obj_set_pos(s_xunit, PLOT_X + PLOT_W - XUNIT_W, XLAB_Y);
    s_msg = mk_label(s_pane_spec, PLOT_X, PLOT_Y + PLOT_H / 2 - 9, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_set_width(s_msg, PLOT_W);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    // #TFT-34: на графике открыт сохранённый спектр — подпись и возврат к живому.
    s_saved_lbl = mk_label(s_pane_spec, PLOT_X + 6, PLOT_Y + 5, &lv_font_montserrat_12, COL_WARN, "");
    lv_obj_set_style_bg_color(s_saved_lbl, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_saved_lbl, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(s_saved_lbl, 4, 0);
    lv_obj_add_flag(s_saved_lbl, LV_OBJ_FLAG_HIDDEN);
    s_live_btn = mk_button(s_pane_spec, PLOT_X + PLOT_W - 70, PLOT_Y + 4, 64, "Live", COL_ACCENT, COL_ACCENT,
                           LV_OPA_30, live_cb, 0);
    lv_obj_add_flag(s_live_btn, LV_OBJ_FLAG_HIDDEN);

    // Панель «Монитор».
    s_pane_mon = mk_pane(scr);
    lv_obj_add_flag(s_pane_mon, LV_OBJ_FLAG_HIDDEN);
    mk_tile(s_pane_mon, 10, "session avg, cps", &s_sess_val, &s_sess_err);
    s_last_cap = mk_tile(s_pane_mon, 10 + TILE_W + 10, "last interval, cps", &s_last_val, &s_last_err);
    s_mon_plot = lv_canvas_create(s_pane_mon);
    lv_canvas_set_buffer(s_mon_plot, s_mon_buf, MCH_W, MCH_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(s_mon_plot, MCH_X, MCH_Y);
    for (int i = 0; i < N_MYLAB; i++) s_mylab[i] = mk_tick(s_pane_mon, MCH_X - 3, LV_TEXT_ALIGN_RIGHT, COL_TICK_X);
    for (int i = 0; i < N_MXLAB; i++) s_mxlab[i] = mk_tick(s_pane_mon, MXLAB_W, LV_TEXT_ALIGN_CENTER, COL_TICK_X);
    // Без часов платы ось X — наборное время от начала сессии; тогда подписываем.
    s_mxunit = mk_tick(s_pane_mon, XUNIT_W, LV_TEXT_ALIGN_RIGHT, COL_AXIS);
    lv_label_set_text(s_mxunit, "elapsed");
    lv_obj_set_pos(s_mxunit, MCH_X + MCH_W - XUNIT_W, MXLAB_Y);
    s_raw_lbl = mk_label(s_pane_mon, MCH_X + 8, MCH_Y + 4, &lv_font_montserrat_12, COL_ACCENT, "CPS 1s");
    s_sma_lbl = mk_label(s_pane_mon, MCH_X + 40, MCH_Y + 4, &lv_font_montserrat_12, COL_SMA, "SMA");
    // Статус записи — как #run-stat веба.
    s_mon_stat = mk_label(s_pane_mon, MCH_X + MCH_W - 208, MCH_Y + 4, &lv_font_montserrat_12, COL_DIM, "");
    lv_obj_set_width(s_mon_stat, 200);
    lv_obj_set_style_text_align(s_mon_stat, LV_TEXT_ALIGN_RIGHT, 0);
    s_mon_msg = mk_label(s_pane_mon, MCH_X, MCH_Y + MCH_H / 2 - 9, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_set_width(s_mon_msg, MCH_W);
    lv_obj_set_style_text_align(s_mon_msg, LV_TEXT_ALIGN_CENTER, 0);

    // #TFT-28: панель «Водопад» — геометрия та же, что у спектра.
    wf_build_lut();
    s_pane_wf = mk_pane(scr);
    lv_obj_add_flag(s_pane_wf, LV_OBJ_FLAG_HIDDEN);
    s_wf_plot = lv_canvas_create(s_pane_wf);
    lv_canvas_set_buffer(s_wf_plot, s_wf_buf, PLOT_W, PLOT_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(s_wf_plot, PLOT_X, PLOT_Y);
    for (int i = 0; i < N_WYLAB; i++) s_wf_ylab[i] = mk_tick(s_pane_wf, PLOT_X - 3, LV_TEXT_ALIGN_RIGHT, COL_TICK_Y);
    for (int i = 0; i < N_XLAB; i++) s_wf_xlab[i] = mk_tick(s_pane_wf, 40, LV_TEXT_ALIGN_CENTER, COL_TICK_X);
    s_wf_xunit = mk_tick(s_pane_wf, XUNIT_W, LV_TEXT_ALIGN_RIGHT, COL_AXIS);
    lv_obj_set_pos(s_wf_xunit, PLOT_X + PLOT_W - XUNIT_W, XLAB_Y);
    // Статус записи (#hud веба) — поверх графика справа сверху, на подложке.
    s_wf_stat = mk_label(s_pane_wf, 0, PLOT_Y + 3, &lv_font_montserrat_12, COL_TXT, "");
    lv_obj_set_style_bg_color(s_wf_stat, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_wf_stat, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(s_wf_stat, 4, 0);
    lv_obj_align(s_wf_stat, LV_ALIGN_TOP_RIGHT, -(TFT_H_RES - PLOT_X - PLOT_W) - 3, PLOT_Y + 3);
    s_wf_msg = mk_label(s_pane_wf, PLOT_X, PLOT_Y + PLOT_H / 2 - 9, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_set_width(s_wf_msg, PLOT_W);
    lv_obj_set_style_text_align(s_wf_msg, LV_TEXT_ALIGN_CENTER, 0);

    // #TFT-29: панель «Conf» — режим сети и окно каналов экрана.
    s_pane_conf = mk_pane(scr);
    lv_obj_add_flag(s_pane_conf, LV_OBJ_FLAG_HIDDEN);
    mk_label(s_pane_conf, 16, 56, &lv_font_montserrat_14, COL_DIM, "Network mode");
    s_net_lbl = mk_label(s_pane_conf, 16, 76, &lv_font_montserrat_12, COL_TXT, "");
    // #ALM-1: четыре кнопки в ряд (Indoor/Outdoor были по 170 px).
    s_net_btn[0] = mk_button(s_pane_conf, 16, 96, 120, "Indoor", COL_ACCENT, COL_TXT,
                             LV_OPA_10, net_btn_cb, 0);
    s_net_btn[1] = mk_button(s_pane_conf, 146, 96, 120, "Outdoor", COL_ACCENT, COL_TXT,
                             LV_OPA_10, net_btn_cb, 1);
    mk_button(s_pane_conf, 276, 96, 90, LV_SYMBOL_WIFI " WiFi", COL_DIM, COL_TXT, LV_OPA_10,
              wifi_open_cb, 0);   // #TFT-41
    mk_button(s_pane_conf, 376, 96, 88, LV_SYMBOL_BELL " Alarm", COL_DIM, COL_TXT, LV_OPA_10,
              alm_open_cb, 0);    // #ALM-1
    mk_rect(s_pane_conf, 16, 144, TFT_H_RES - 32, 1, COL_BD);
    mk_label(s_pane_conf, 16, 156, &lv_font_montserrat_14, COL_DIM, "Display window, channels");
    s_chw_lbl = mk_label(s_pane_conf, 0, 156, &lv_font_montserrat_16, COL_ACCENT, "");
    lv_obj_set_width(s_chw_lbl, 150);
    lv_obj_set_style_text_align(s_chw_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_x(s_chw_lbl, TFT_H_RES - 16 - 150);
    s_chw_slider = lv_slider_create(s_pane_conf);
    lv_slider_set_mode(s_chw_slider, LV_SLIDER_MODE_RANGE);
    lv_slider_set_range(s_chw_slider, 0, CH_WIN_MAX);
    lv_obj_set_size(s_chw_slider, TFT_H_RES - 72, 8);
    lv_obj_set_pos(s_chw_slider, 36, 204);
    lv_obj_set_style_bg_color(s_chw_slider, lv_color_hex(COL_BD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_chw_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_chw_slider, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_chw_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_chw_slider, lv_color_hex(COL_ACCENT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_chw_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_color(s_chw_slider, lv_color_hex(COL_TXT), LV_PART_KNOB);
    lv_obj_set_style_border_width(s_chw_slider, 2, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_chw_slider, 9, LV_PART_KNOB);   // ручки ~26 px — под палец
    lv_obj_set_style_radius(s_chw_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_chw_slider, 18);
    lv_obj_add_event_cb(s_chw_slider, chw_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_chw_slider, chw_slider_cb, LV_EVENT_RELEASED, NULL);
    mk_label(s_pane_conf, 36, 226, &lv_font_montserrat_12, COL_TICK_X, "0");
    lv_obj_t *mx = mk_label(s_pane_conf, 0, 226, &lv_font_montserrat_12, COL_TICK_X, "8191");
    lv_obj_align(mx, LV_ALIGN_TOP_RIGHT, -36, 226);
    mk_label(s_pane_conf, 16, 250, &lv_font_montserrat_12, COL_DIM,
             "Spectrum and waterfall on this display. Minimum width 100 channels.");

    // #TFT-34: экран «Files» — поверх панелей; папки сверху, список — таблица.
    s_files = mk_rect(scr, 0, FILES_Y, TFT_H_RES, BAR_Y - 1 - FILES_Y, COL_BG);
    lv_obj_add_flag(s_files, LV_OBJ_FLAG_CLICKABLE);   // касания не проходят к панели под ним
    // #SD-7: папки CSV нет — копии спектров в CSV больше не пишутся.
    static const char *const DIRS[3] = {"Spectra", "Monitor", "Waterfall"};
    for (int i = 0; i < 3; i++)
        s_files_dirbtn[i] = mk_button(s_files, 6 + i * 88, 4, 84, DIRS[i], COL_ACCENT, COL_TXT, LV_OPA_10,
                                      files_open_cb, i);
    mk_button(s_files, TFT_H_RES - 6 - 80, 4, 80, LV_SYMBOL_CLOSE "  Close", COL_DIM, COL_TXT, LV_OPA_10,
              files_close_cb, 0);
    s_files_tbl = lv_table_create(s_files);
    lv_obj_set_pos(s_files_tbl, 0, 40);
    lv_obj_set_size(s_files_tbl, TFT_H_RES, BAR_Y - 1 - FILES_Y - 40);
    lv_table_set_col_cnt(s_files_tbl, 2);
    lv_table_set_col_width(s_files_tbl, 0, 296);
    lv_table_set_col_width(s_files_tbl, 1, TFT_H_RES - 296);
    lv_obj_set_style_bg_color(s_files_tbl, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_files_tbl, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_files_tbl, 0, 0);
    lv_obj_set_style_pad_all(s_files_tbl, 0, 0);
    lv_obj_set_style_radius(s_files_tbl, 0, 0);
    lv_obj_set_style_bg_color(s_files_tbl, lv_color_hex(COL_PANEL), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_files_tbl, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_files_tbl, lv_color_hex(COL_BD), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_files_tbl, lv_color_hex(COL_TXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_files_tbl, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_files_tbl, lv_color_hex(COL_BD), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_files_tbl, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_side(s_files_tbl, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(s_files_tbl, 9, LV_PART_ITEMS);   // строка ~36 px — под палец
    lv_obj_set_style_pad_hor(s_files_tbl, 8, LV_PART_ITEMS);
    lv_obj_add_event_cb(s_files_tbl, files_tbl_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(s_files, LV_OBJ_FLAG_HIDDEN);

    // Нижняя панель: вкладки слева, Start/Stop справа. На вкладке Spectrum —
    // команды прибору (-sta / -sto, как страница спектра веба), на Monitor —
    // старт/стоп общей сессии мониторинга (#MON-2, как страница мониторинга).
    mk_rect(scr, 0, BAR_Y - 1, TFT_H_RES, 1, COL_BD);
    mk_rect(scr, 0, BAR_Y, TFT_H_RES, TFT_V_RES - BAR_Y, COL_PANEL);
    // #TFT-27: вкладки и действия — две прозрачные группы на одном месте.
    // Группы во весь экран, дети — в экранных координатах: LVGL ищет касание в
    // детях только внутри родителя, иначе запас зоны касания (10 px вверх) пропал бы.
    s_tabs = mk_rect(scr, 0, 0, TFT_H_RES, TFT_V_RES, 0);
    lv_obj_set_style_bg_opa(s_tabs, LV_OPA_TRANSP, 0);
    const int by = BAR_Y + (TFT_V_RES - BAR_Y - BTN_H) / 2;
    // #TFT-36: вкладки — кнопки на местах Stop/Clear/Save/Files (ряд за «•••»),
    // чтобы при переключении кнопки вставали одна на другую.
    static const char *const TABS[4] = {"Spectrum", "Monitor", "Waterfall", "Conf"};
    for (int i = 0; i < 4; i++) {
        s_tab_btn[i] = mk_button(s_tabs, ACT_X0 + i * ACT_STEP, by, ACT_W, TABS[i], COL_DIM, COL_TXT,
                                 LV_OPA_10, tab_cb, i);
        btn_select(s_tab_btn[i], i == 0);
    }
    s_acts = mk_rect(scr, 0, 0, TFT_H_RES, TFT_V_RES, 0);   // как s_tabs: во весь экран
    lv_obj_set_style_bg_opa(s_acts, LV_OPA_TRANSP, 0);
    // #TFT-40: Stop в этом ряду убран — основная кнопка сама Start <-> Stop (#TFT-37),
    // при идущем наборе было две кнопки Stop.
    mk_button(s_acts, ACT_X0, by, ACT_W, LV_SYMBOL_TRASH " Clear", COL_WARN, COL_WARN,
              LV_OPA_10, clear_cb, 0);
    s_act_save = mk_button(s_acts, ACT_X0 + ACT_STEP, by, ACT_W, LV_SYMBOL_SAVE " Save",
                           COL_ACCENT, COL_ACCENT, LV_OPA_10, btn_cb, TFT_CMD_SAVE);
    // #TFT-34: просмотр файлов карты и сохранённых спектров (открывает папку Spectra).
    mk_button(s_acts, ACT_X0 + 2 * ACT_STEP, by, ACT_W, LV_SYMBOL_DIRECTORY " Files", COL_DIM, COL_TXT,
              LV_OPA_10, files_open_cb, 0);
    lv_obj_add_flag(s_acts, LV_OBJ_FLAG_HIDDEN);
    s_btn_start = mk_button(scr, START_X, by, BTN_W, LV_SYMBOL_PLAY "  Start", COL_ACCENT, COL_ACCENT,
                            LV_OPA_20, start_cb, 0);   // #TFT-37: Start <-> Stop
    // «•••» — U+2022 есть в шрифтах (#TFT-18).
    s_btn_more = mk_button(scr, MORE_X, by, MORE_W, "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2", COL_DIM,
                           COL_TXT, LV_OPA_10, more_cb, 0);

    // #TFT-27: подтверждение Clear — модальное окно поверх всего.
    s_confirm = mk_rect(lv_layer_top(), 0, 0, TFT_H_RES, TFT_V_RES, COL_BG);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_70, 0);
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_CLICKABLE);   // касания мимо окна не уходят вниз
    lv_obj_t *box = mk_rect(s_confirm, 90, 95, 300, 130, COL_PANEL);
    lv_obj_set_style_border_color(box, lv_color_hex(COL_BD), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    s_confirm_txt = mk_label(s_confirm, 90, 125, &lv_font_montserrat_16, COL_TXT, "");
    lv_obj_set_width(s_confirm_txt, 300);
    lv_obj_set_style_text_align(s_confirm_txt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *ok = mk_button(s_confirm, 110, 175, 120, LV_SYMBOL_TRASH "  Clear", COL_WARN, COL_WARN, LV_OPA_20,
              confirm_cb, TFT_CMD_CLEAR);
    mk_button(s_confirm, 250, 175, 120, "Cancel", COL_DIM, COL_TXT, LV_OPA_10, confirm_cb, 0);
    s_confirm_ok_lbl = lv_obj_get_child(ok, 0);   // надпись меняют clear_cb / net_btn_cb
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);

    // #MNT-1: вид «на трубе» — непрозрачный слой во весь экран поверх панелей и
    // кнопок (касания вниз не пропускает); подсказки (toast) — над ним.
    s_mnt = mk_rect(scr, 0, 0, MNT_PW, MNT_PH, COL_BG);   // в портретных координатах
    lv_obj_add_flag(s_mnt, LV_OBJ_FLAG_CLICKABLE);
    // #MNT-3 (запрос 2026-10-13): число CPS — на всю высоту полосы до графика,
    // самым крупным шрифтом, что влезает по ширине (mnt_layout_top); батарея,
    // подпись CPS и доза — узкой колонкой справа (строка под батарею съедала место).
    s_mnt_cps  = mk_label(s_mnt, 8, 0, &font_cps104, COL_ACCENT, "--");
    s_mnt_batt = mk_label(s_mnt, 0, 0, &lv_font_montserrat_16, COL_CHIP_OK, "");
    lv_obj_align(s_mnt_batt, LV_ALIGN_TOP_RIGHT, -8, 6);
    s_mnt_dot = mk_rect(s_mnt, 0, 0, 10, 10, COL_DOT_OFF);
    lv_obj_set_style_radius(s_mnt_dot, LV_RADIUS_CIRCLE, 0);
    s_mnt_unit = mk_label(s_mnt, 0, 0, &lv_font_montserrat_16, COL_DIM, "CPS");
    lv_obj_align(s_mnt_unit, LV_ALIGN_TOP_RIGHT, -8, 32);
    s_mnt_dose = mk_label(s_mnt, 0, 0, &lv_font_montserrat_14, COL_TXT, "");
    lv_obj_align(s_mnt_dose, LV_ALIGN_TOP_RIGHT, -8, 58);
    lv_obj_add_flag(s_mnt_dose, LV_OBJ_FLAG_HIDDEN);
    s_mnt_mon = lv_canvas_create(s_mnt);
    lv_canvas_set_buffer(s_mnt_mon, s_mnt_mon_buf, MNT_W, MNT_MON_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(s_mnt_mon, MNT_X, MNT_MON_Y);
    lv_obj_add_flag(s_mnt_mon, LV_OBJ_FLAG_CLICKABLE);   // #MNT-2: двойной тап — старт мониторинга
    lv_obj_add_flag(s_mnt_mon, LV_OBJ_FLAG_PRESS_LOCK);   // #MNT-5: свайп справа налево — очистка
    lv_obj_add_event_cb(s_mnt_mon, mnt_swipe_ev_cb, LV_EVENT_PRESSED, (void *)1);
    lv_obj_add_event_cb(s_mnt_mon, mnt_swipe_ev_cb, LV_EVENT_RELEASED, (void *)1);
    lv_obj_add_event_cb(s_mnt_mon, mnt_swipe_ev_cb, LV_EVENT_SHORT_CLICKED, (void *)1);
    s_mnt_mhi = mk_label(s_mnt, MNT_X + 5, MNT_MON_Y + 3, &lv_font_montserrat_12, COL_TICK_X, "");
    s_mnt_mlo = mk_label(s_mnt, MNT_X + 5, MNT_MON_Y + MNT_MON_H - 17, &lv_font_montserrat_12, COL_TICK_X, "");
    s_mnt_sma = mk_label(s_mnt, 0, 0, &lv_font_montserrat_12, COL_SMA, "SMA");
    lv_obj_align(s_mnt_sma, LV_ALIGN_TOP_RIGHT, -(MNT_X + 6), MNT_MON_Y + 3);
    s_mnt_raw = mk_label(s_mnt, 0, MNT_MON_Y + 3, &lv_font_montserrat_12, COL_ACCENT, "CPS 1s");
    s_mnt_mmsg = mk_label(s_mnt, MNT_X, MNT_MON_Y + MNT_MON_H / 2 - 9, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_set_width(s_mnt_mmsg, MNT_W);
    lv_obj_set_style_text_align(s_mnt_mmsg, LV_TEXT_ALIGN_CENTER, 0);
    s_mnt_spec = lv_canvas_create(s_mnt);
    lv_canvas_set_buffer(s_mnt_spec, s_mnt_spec_buf, MNT_W, MNT_SPEC_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(s_mnt_spec, MNT_X, MNT_SPEC_Y);
    // Двойной тап — Lin/Log, как на Spectrum; свайп справа налево — очистка (#MNT-3).
    lv_obj_add_flag(s_mnt_spec, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(s_mnt_spec, mnt_swipe_ev_cb, LV_EVENT_PRESSED, (void *)0);
    lv_obj_add_event_cb(s_mnt_spec, mnt_swipe_ev_cb, LV_EVENT_RELEASED, (void *)0);
    lv_obj_add_event_cb(s_mnt_spec, mnt_swipe_ev_cb, LV_EVENT_SHORT_CLICKED, (void *)0);
    s_mnt_sinfo = mk_label(s_mnt, 0, 0, &lv_font_montserrat_12, COL_TICK_X, "");
    lv_obj_align(s_mnt_sinfo, LV_ALIGN_TOP_RIGHT, -(MNT_X + 6), MNT_SPEC_Y + 3);
    s_mnt_smsg = mk_label(s_mnt, MNT_X, MNT_SPEC_Y + MNT_SPEC_H / 2 - 9, &lv_font_montserrat_14, COL_DIM, "");
    lv_obj_set_width(s_mnt_smsg, MNT_W);
    lv_obj_set_style_text_align(s_mnt_smsg, LV_TEXT_ALIGN_CENTER, 0);
    if (!s_mount) lv_obj_add_flag(s_mnt, LV_OBJ_FLAG_HIDDEN);

    // Результат нажатия кнопки — поверх панелей, на пару секунд.
    s_toast = mk_label(scr, 0, 0, &lv_font_montserrat_14, COL_TXT, "");
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(COL_BD), 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_radius(s_toast, 4, 0);
    lv_obj_set_style_pad_hor(s_toast, 10, 0);
    lv_obj_set_style_pad_ver(s_toast, 4, 0);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, TOAST_Y);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    // #TFT-21: оверлей калибровки тача — на верхнем слое, поверх всего, скрыт.
    // Кликабелен на весь экран: во время калибровки touch_process() отдаёт LVGL
    // центр экрана, так что отпускание пальца всегда приходит сюда.
    s_cal_layer = mk_rect(lv_layer_top(), 0, 0, TFT_H_RES, TFT_V_RES, COL_BG);
    lv_obj_add_flag(s_cal_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cal_layer, cal_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_cal_layer, cal_release_cb, LV_EVENT_RELEASED, NULL);
    s_cal_h = mk_rect(s_cal_layer, 0, 0, 41, 2, COL_SMA);
    s_cal_v = mk_rect(s_cal_layer, 0, 0, 2, 41, COL_SMA);
    s_cal_txt = mk_label(s_cal_layer, 0, 92, &lv_font_montserrat_16, COL_TXT, "");
    lv_obj_set_width(s_cal_txt, TFT_H_RES);
    lv_obj_set_style_text_align(s_cal_txt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_cal_layer, LV_OBJ_FLAG_HIDDEN);

    lv_scr_load(scr);
    if (s_mount) mount_set(true);   // #MNT-1: старт в портрете (режим из NVS)
    lvgl_port_unlock();
    return true;
}

// ---------------- экран: отрисовка ----------------

#define PX(x, y)  s_plot_buf[(y) * PLOT_W + (x)]
#define MPX(x, y) s_mon_buf[(y) * MCH_W + (x)]

// Строка холста высотой h для значения: yMap() веба, 0 — верх, h-1 — низ.
// log: [lo..hi] — показатели степени; lin: [0..hi] — значения (#TFT-24).
static int y_row_h(float v, bool lg, float lo, float hi, int h)
{
    if (v <= 0) return h - 1;
    float f;
    if (lg) {
        float l = log10f(v);
        if (l <= lo) return h - 1;
        if (l >= hi) return 0;
        f = (l - lo) / (hi - lo);
    } else {
        f = v / hi;
        if (f >= 1) return 0;
    }
    return (h - 1) - (int)(f * (h - 1) + 0.5f);
}

static int y_row(float v, bool lg, float lo, float hi) { return y_row_h(v, lg, lo, hi, PLOT_H); }

// Шкала Y спектра: Lin веба — 0..пик, деления niceStep(mx, 6); Log веба (#UI-36) —
// верх пик + 0.15 декады, пол — ОДИН отсчёт за время набора (не меньше декады),
// деления — декады. Общая для вкладки Spectrum и вида «на трубе» (#MNT-1).
static void spec_yscale(float mx, float tt, bool lg, float *ylo, float *yhi,
                        double *t0, double *tstep, double *tend)
{
    *ylo = 0; *yhi = mx;
    *t0 = 0; *tstep = nice_step(mx, 6); *tend = mx * 1.01;
    if (lg) {
        *yhi = log10f(mx) + 0.15f;
        *ylo = -log10f(tt);
        if (*ylo >= *yhi - 0.5f) *ylo = *yhi - 1;
        *t0 = ceilf(*ylo);
        *tstep = 1;
        *tend = floorf(*yhi);
    }
}

static void fmt_group(uint32_t v, char *out, size_t n)   // "2 071 344"
{
    char t[12];
    int len = snprintf(t, sizeof t, "%lu", (unsigned long)v), o = 0;
    for (int i = 0; i < len && o + 2 < (int)n; i++) {
        if (i > 0 && (len - i) % 3 == 0) out[o++] = ' ';
        out[o++] = t[i];
    }
    out[o] = '\0';
}

static void fmt_cps(double v, char *out, size_t n)   // fmtCps() из web/monitor.html
{
    if (!isfinite(v))   snprintf(out, n, "--");
    else if (v >= 100)  snprintf(out, n, "%.1f", v);
    else if (v >= 1)    snprintf(out, n, "%.3f", v);
    else                snprintf(out, n, "%.4f", v);
}

// Наборное время от начала сессии для делений оси X монитора.
static void fmt_elapsed(uint32_t s, uint32_t step, char *out, size_t n)
{
    if (step < 60)        snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    else if (s < 3600)    snprintf(out, n, "%lum", (unsigned long)(s / 60));
    else                  snprintf(out, n, "%luh%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
}

// xTickStep() веба в режиме «авто»: первый шаг из ряда >= span/6, секунды.
static uint32_t x_tick_step(uint32_t span)
{
    static const uint32_t M[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200,
                                 10800, 21600, 43200, 86400, 172800, 604800};
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++)
        if (M[i] * 6 >= span) return M[i];
    return M[sizeof M / sizeof M[0] - 1];
}

// xTickLabel() веба: HH:MM, +:SS при шаге < 1 мин, +дата при охвате >= суток.
// local — время уже в поясе отображения (UTC + смещение).
static void fmt_wall(int64_t local, uint32_t step, uint32_t span, char *out, size_t n)
{
    time_t t = (time_t)local;
    struct tm tm;
    gmtime_r(&t, &tm);
    if (span >= 86400)
        snprintf(out, n, "%02d.%02d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
    else if (step < 60)
        snprintf(out, n, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    else
        snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

static void draw_frame(lv_color_t *buf, int w, int h)
{
    for (int x = 0; x < w; x++) { buf[x] = s_c_frame; buf[(h - 1) * w + x] = s_c_frame; }
    for (int y = 0; y < h; y++) { buf[y * w] = s_c_frame; buf[y * w + w - 1] = s_c_frame; }
}

// Шапка — общая для обеих панелей. Вызывать под lvgl_port_lock.
// #TFT-22: статус прибора — ровно как #dev-chip веба (usb_connected), и значок
// батареи — как #batt-chip: заливка по %, цвет <20 красный, <50 жёлтый.
// Раньше здесь была точка «идут данные за 3 с»: после отключения старый спектр
// остаётся в памяти, а без набора данные не меняются — подключение/отключение
// на ней не отражалось. Вызывать под lvgl_port_lock.
// IP платы для шапки: адрес WiFi-клиента, в полевой/настроечной точке доступа —
// её адрес (#TFT-23). Пусто — сети нет.
static void get_ip(char *out, size_t n)
{
    static const char *const IFS[2] = {"WIFI_STA_DEF", "WIFI_AP_DEF"};
    out[0] = '\0';
    for (int i = 0; i < 2 && !out[0]; i++) {
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey(IFS[i]);
        esp_netif_ip_info_t ip;
        if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr != 0)
            esp_ip4addr_ntoa(&ip.ip, out, n);
    }
}

// #TFT-25: строка статуса справа налево: [точка] [DEV] ... [батарея][+] [%].
// Ширина подписей переменная — берём фактическую после lv_obj_update_layout().
static void layout_status(void)
{
    int x = TFT_H_RES - HDR_R_PAD;
    if (!lv_obj_has_flag(s_batt_pct, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_update_layout(s_batt_pct);
        x -= lv_obj_get_width(s_batt_pct);
        lv_obj_set_x(s_batt_pct, x);
        x -= 4 + 2 + 18;   // зазор, «плюс», корпус
        lv_obj_set_x(s_batt, x);
        x -= 10;           // до статуса прибора
    }
    lv_obj_update_layout(s_dev_lbl);
    x -= lv_obj_get_width(s_dev_lbl);
    lv_obj_set_x(s_dev_lbl, x);
    lv_obj_set_x(s_dot, x - 5 - 7);
}

// #TFT-39: цвет точки DEV по s_dot_conn / s_dot_acq. Под lvgl_port_lock.
static void dot_apply(void)
{
    uint32_t col = !s_dot_conn ? COL_DOT_OFF : s_dot_acq ? COL_RED : COL_ACCENT;
    lv_opa_t opa = s_dot_conn && s_dot_acq && !s_dot_blink ? LV_OPA_20 : LV_OPA_COVER;
    lv_obj_t *const dots[2] = { s_dot, s_mnt_dot };   // #MNT-1: та же точка в виде «на трубе»
    for (int i = 0; i < 2; i++) {
        if (!dots[i]) continue;
        lv_obj_set_style_bg_color(dots[i], lv_color_hex(col), 0);
        lv_obj_set_style_bg_opa(dots[i], opa, 0);
    }
}

static void render_rec(bool acq)
{
    s_dot_acq = acq;
    dot_apply();
}

// #TFT-37: Start (бирюзовая, как была) или Stop (красная, как Stop за «•••»).
// Под lvgl_port_lock.
static void start_btn_set(bool stop)
{
    s_btn_is_stop = stop;
    uint32_t col = stop ? COL_RED : COL_ACCENT;
    lv_obj_set_style_bg_color(s_btn_start, lv_color_hex(col), 0);
    lv_obj_set_style_border_color(s_btn_start, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(s_btn_start, stop ? LV_OPA_10 : LV_OPA_20, 0);
    lv_obj_t *l = lv_obj_get_child(s_btn_start, 0);
    lv_label_set_text(l, stop ? LV_SYMBOL_STOP "  Stop" : LV_SYMBOL_PLAY "  Start");
    lv_obj_set_style_text_color(l, lv_color_hex(stop ? COL_RED_TXT : COL_ACCENT), 0);
}

static void render_status(bool conn, const battery_status_t *bs, const char *ip)
{
    lv_label_set_text(s_ip, ip[0] ? ip : "no network");
    s_dot_conn = conn;   // #TFT-39: цвет — dot_apply()
    dot_apply();
    lv_label_set_text(s_dev_lbl, conn ? "DEV" : "NO DEV");
    lv_obj_set_style_text_color(s_dev_lbl, lv_color_hex(conn ? COL_CHIP_OK : COL_WARN), 0);
    if (!bs->present) {
        lv_obj_add_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_batt_pct, LV_OBJ_FLAG_HIDDEN);
        layout_status();
        return;
    }
    int pct = bs->percent < 0 ? 0 : bs->percent > 100 ? 100 : bs->percent;
    lv_obj_set_width(s_batt_fill, 14 * pct / 100);   // внутренняя ширина корпуса 18 - 2*(рамка+отступ)
    lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(pct < 20 ? COL_RED_TXT : pct < 50 ? COL_WARN : COL_ACCENT), 0);
    // Молния веба (U+26A1) — в шрифте её нет, есть символ LVGL «заряд».
    lv_label_set_text_fmt(s_batt_pct, "%s%d%%", bs->charging ? LV_SYMBOL_CHARGE : "", pct);
    lv_obj_clear_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_batt_pct, LV_OBJ_FLAG_HIDDEN);
    layout_status();
}

static void render_header(bool valid)
{
    char txt[16];
    if (valid) {
        const spectrum_data_t *sp = s_snap;
        lv_label_set_text_fmt(s_cps_val, "%lu", (unsigned long)sp->cps);
        uint32_t t = sp->total_time_sec;
        lv_label_set_text_fmt(s_time_val, "%02lu:%02lu:%02lu",
                              (unsigned long)(t / 3600), (unsigned long)(t / 60 % 60), (unsigned long)(t % 60));
        fmt_group(sp->total_counts, txt, sizeof txt);
        lv_label_set_text(s_total_val, txt);
        // Доза — как в вебе: CPS * коэффициент прибора, только если он задан.
        if (spectrogram_get_dose_k() > 0) {
            snprintf(txt, sizeof txt, "%.3f", (double)(sp->cps * spectrogram_get_dose_k()));   // %f — только newlib
            lv_label_set_text(s_dose_val, txt);
        }
    } else {
        lv_label_set_text(s_cps_val, "--");
        lv_label_set_text(s_time_val, "--:--:--");
        lv_label_set_text(s_total_val, "--");
        lv_label_set_text(s_dose_val, "--");
    }
    // #SYS-1: колонка x 308 — доза, если коэффициент задан, иначе загрузка процессоров.
    bool dose = spectrogram_get_dose_k() > 0;
    lv_obj_t *const on[2]  = { dose ? s_dose_cap : s_cpu_cap, dose ? s_dose_val : s_cpu_val };
    lv_obj_t *const off[2] = { dose ? s_cpu_cap : s_dose_cap, dose ? s_cpu_val : s_dose_val };
    for (int i = 0; i < 2; i++) {
        lv_obj_clear_flag(on[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(off[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_update_layout(s_cps_val);   // "CPS" сразу за числом переменной ширины
    lv_obj_align_to(s_cps_unit, s_cps_val, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, -5);
}

// Панель «Спектр» по s_snap. Слои в порядке draw() веба: фон, сетка, заливка,
// линия, рамка; затем подписи. Вызывать под lvgl_port_lock.
static void render_spectrum(bool valid, const char *msg)
{
    char txt[16];
    int ny = 0, nx = 0;
    for (int i = 0; i < PLOT_W * PLOT_H; i++) s_plot_buf[i] = s_c_bg;

    if (valid) {
        const spectrum_data_t *sp = s_saved_on ? s_saved : s_snap;   // #TFT-34: открытый файл или живой
        float tt = sp->total_time_sec > 0 ? (float)sp->total_time_sec : 1.0f;
        float mx = build_columns(sp, tt, PLOT_W);
        if (mx <= 0) mx = 1;
        bool lg = s_spec_log;
        float ylo, yhi;
        double t0, tstep, tend;
        spec_yscale(mx, tt, lg, &ylo, &yhi, &t0, &tstep, &tend);
        for (double t = t0; t <= tend && ny < N_YLAB; t += tstep) {
            double v = lg ? pow(10, t) : t;
            int y = y_row((float)v, lg, ylo, yhi);
            for (int x = 0; x < PLOT_W; x++) PX(x, y) = s_c_grid;
            fmt_y(v, txt, sizeof txt);
            lv_label_set_text(s_ylab[ny], txt);
            lv_obj_set_pos(s_ylab[ny], 0, PLOT_Y + y - 7);
            lv_obj_clear_flag(s_ylab[ny], LV_OBJ_FLAG_HIDDEN);
            ny++;
        }

        // Ось X: кэВ по калибровке прибора, без неё — номера каналов.
        const int range = SPEC_CH_TO - SPEC_CH_FROM + 1;
        bool kev = sp->calib_valid;
        double lo = kev ? ch2kev(sp, SPEC_CH_FROM) : SPEC_CH_FROM;
        double hi = kev ? ch2kev(sp, SPEC_CH_TO + 1) : SPEC_CH_TO + 1;
        double step = nice_step(hi, 6);   // веб берёт 8 делений на 1200 px, здесь 440 px
        for (double k = 0; k <= hi * 1.01 && nx < N_XLAB; k += step) {
            if (k < lo) continue;
            double ch = kev ? kev2ch(sp, k) : k;
            int x = (int)((ch - SPEC_CH_FROM) / range * PLOT_W + 0.5);
            if (x < 0 || x >= PLOT_W) continue;
            for (int y = 0; y < PLOT_H; y++) PX(x, y) = s_c_grid;
            if (x > PLOT_W - XUNIT_W - 14) continue;   // не наезжать на подпись оси
            snprintf(txt, sizeof txt, "%.0f", k);
            lv_label_set_text(s_xlab[nx], txt);
            lv_obj_set_pos(s_xlab[nx], PLOT_X + x - 20, XLAB_Y);
            lv_obj_clear_flag(s_xlab[nx], LV_OBJ_FLAG_HIDDEN);
            nx++;
        }
        lv_label_set_text(s_xunit, kev ? "keV" : "Channel");

        // Спектр: заливка 12% от точки до низа (сквозь неё видна сетка) и линия.
        int prev = y_row(s_col[0], lg, ylo, yhi);
        for (int x = 0; x < PLOT_W; x++) {
            int y = y_row(s_col[x], lg, ylo, yhi);
            if (y < PLOT_H - 1)
                for (int yy = y; yy < PLOT_H; yy++)
                    PX(x, yy) = PX(x, yy).full == s_c_grid.full ? s_c_fill_grid : s_c_fill;
            int a = prev < y ? prev : y, b = prev < y ? y : prev;
            for (int yy = a; yy <= b; yy++) PX(x, yy) = s_c_line;
            prev = y;
        }
        lv_obj_clear_flag(s_xunit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_xunit, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_msg, msg ? msg : "");
        lv_obj_clear_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = ny; i < N_YLAB; i++) lv_obj_add_flag(s_ylab[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = nx; i < N_XLAB; i++) lv_obj_add_flag(s_xlab[i], LV_OBJ_FLAG_HIDDEN);
    draw_frame(s_plot_buf, PLOT_W, PLOT_H);
    lv_obj_invalidate(s_plot);
}

// ---------------- мониторинг: данные ----------------

static bool mon_close(void)   // closeBucket() из web/monitor.html
{
    if (!s_bucket_open) return false;
    s_bucket_open = false;
    if (s_bucket.dur == 0) return false;
    if (s_iv_n == MON_CAP) {   // только при интервале 1 с и полном кольце: старейшую четверть — вон
        memmove(s_iv, s_iv + MON_CAP / 4, (MON_CAP - MON_CAP / 4) * sizeof(*s_iv));
        s_iv_n = MON_CAP - MON_CAP / 4;
    }
    s_iv[s_iv_n++] = s_bucket;
    s_sess_counts += s_bucket.counts;
    s_sess_dur    += s_bucket.dur;
    // #SD-3: тот же закрытый интервал — строкой в файл сессии на карте.
    sd_log_mon_interval(s_bucket.dev_start, s_bucket.dev_end, s_bucket.dur, s_bucket.counts, s_bucket.up);
    return true;
}

static bool mon_push(const monitor_sample_t *m)   // pushBase()
{
    if (!s_bucket_open) {
        s_bucket = (mon_iv_t){ .dev_start = m->end_sec - m->dur, .dev_end = m->end_sec };
        s_bucket_open = true;
        if (s_iv_n == 0 && s_sess_dur == 0) s_sess_t0 = s_bucket.dev_start;
    }
    s_bucket.dur    += m->dur;
    s_bucket.counts += m->counts;
    s_bucket.dev_end = m->end_sec;
    s_bucket.up      = m->up_sec;
    uint32_t iv = s_ms.interval_s ? s_ms.interval_s : 1;
    return s_bucket.dur >= iv ? mon_close() : false;
}

static void mon_reset(void)
{
    s_iv_n = 0;
    s_bucket_open = false;
    s_sess_counts = s_sess_dur = 0;
}

// Забрать новые секундные сэмплы и сессию из monitor.c — syncSeries() веба.
// true — есть что перерисовать. Только из tft_task: рисует её же render_monitor().
static bool mon_pull(void)
{
    bool changed = false;
    for (int hop = 0; hop < 200; hop++) {   // перестройка полной сессии (6 ч) — ~85 кусков
        const monitor_session_t *o = &s_ms;
        bool collecting = s_have_key && o->start_seq && (o->running || s_mon_seq < o->stop_seq);
        uint32_t first, next, epoch;
        monitor_session_t ss;
        size_t n = monitor_copy_since(collecting ? s_mon_seq : UINT32_MAX - 1, s_mon_tmp, MON_CHUNK,
                                      &first, &next, &epoch, &ss);
        if (!s_have_key || epoch != s_key_epoch || ss.rev != s_key_rev) {
            // Новая сессия (Старт/Очистить из веба или с экрана, сброс прибора,
            // старт платы) — ответ не годится, перезапрос с её начала.
            s_have_key  = true;
            s_key_epoch = epoch;
            s_key_rev   = ss.rev;
            s_ms        = ss;
            mon_reset();
            sd_log_mon_new_session(ss.running && ss.start_seq, ss.interval_s);   // #SD-3: новый файл
            s_mon_seq = ss.start_seq ? ss.start_seq - 1 : next - 1;
            changed = true;
            continue;
        }
        if (ss.running != s_ms.running || ss.sma_win != s_ms.sma_win || ss.interval_s != s_ms.interval_s)
            changed = true;
        s_ms = ss;
        if (!ss.start_seq) break;
        uint32_t lim = ss.running ? UINT32_MAX : ss.stop_seq;
        bool done = false;
        for (size_t i = 0; i < n; i++) {
            uint32_t seq = first + (uint32_t)i;
            if (seq <= s_mon_seq) continue;
            if (seq > lim) { done = true; break; }   // хвост за Стопом — не наш
            s_mon_seq = seq;
            changed |= mon_push(&s_mon_tmp[i]);
        }
        if (!ss.running && (s_mon_seq >= ss.stop_seq || first > ss.stop_seq)) done = true;
        if (done || s_mon_seq + 1 >= next) break;
    }
    if (!s_ms.running && s_bucket_open) s_bucket_open = false;   // как Стоп в вебе
    if (!s_ms.running) sd_log_mon_stop();   // #SD-3: итог сессии и закрыть файл (повторно — ничего)
    return changed;
}

// ---------------- мониторинг: отрисовка ----------------

// Ряд графика (s_iv_n > 0): последние MON_VIEW интервалов -> s_m_raw, SMA -> s_m_sma,
// шкала Y по ним +8 %. Общий для вкладки Monitor и вида «на трубе» (#MNT-1).
// Возвращает число точек; start — индекс первой в s_iv.
static int mon_series(int win, int *start_out, float *mn_out, float *mx_out)
{
    float *raw = s_m_raw, *sma = s_m_sma;
    int start = s_iv_n > MON_VIEW ? s_iv_n - MON_VIEW : 0, cnt = s_iv_n - start;
    // SMA trailing по всему ряду (smaAll веба): скользящая сумма от начала окна.
    // #MON-5: в сумме до start — win интервалов [start-win, start-1], а не win-1:
    // первая итерация вычитает s_iv[start-win]. Было start-win+1 — после 200
    // интервалов (MON_VIEW) сумма теряла одно значение, SMA шла ниже raw на 1/win.
    int a0 = start - win < 0 ? 0 : start - win;
    double acc = 0;
    for (int j = a0; j < start; j++) acc += (double)s_iv[j].counts / s_iv[j].dur;
    float mn = 1e30f, mx = -1e30f;
    for (int k = 0; k < cnt; k++) {
        int i = start + k;
        double v = (double)s_iv[i].counts / s_iv[i].dur;   // прибавляем ровно то, что потом вычтем
        raw[k] = (float)v;
        acc += v;
        if (i - win >= 0) acc -= (double)s_iv[i - win].counts / s_iv[i - win].dur;
        sma[k] = (float)(acc / (i + 1 < win ? i + 1 : win));
        mn = fminf(mn, fminf(raw[k], sma[k]));
        mx = fmaxf(mx, fmaxf(raw[k], sma[k]));
    }
    if (mx - mn < 1e-9f) mx = mn + 1;
    float pad = (mx - mn) * 0.08f;
    *start_out = start;
    *mn_out = mn - pad;
    *mx_out = mx + pad;
    return cnt;
}

// Панель «Монитор». Формулы closeBucket()/drawChart() веба: cps = ΣN/Σt,
// погрешность 100/√ΣN %, SMA — скользящее по всему ряду, масштаб Y — по
// видимым точкам и SMA +8%, ось X — настенное время платы в поясе из веба
// (без часов — наборное время от начала сессии). Вызывать под lvgl_port_lock.
static void render_monitor(const char *msg)
{
    float *raw = s_m_raw, *sma = s_m_sma;
    lv_point_t *pr = s_m_pr, *ps = s_m_ps;
    char txt[24];
    int ny = 0, nx = 0;
    for (int i = 0; i < MCH_W * MCH_H; i++) s_mon_buf[i] = s_c_bg;

    int win = s_ms.sma_win < MON_SMA_MIN ? MON_SMA_MIN : s_ms.sma_win;
    snprintf(txt, sizeof txt, "SMA %d", win);
    lv_label_set_text(s_sma_lbl, txt);
    // #MON-6: «CPS Ns» по интервалу сессии; SMA — правее по фактической ширине (+10 px).
    lv_label_set_text_fmt(s_raw_lbl, "CPS %us", (unsigned)(s_ms.interval_s ? s_ms.interval_s : 1));
    lv_obj_update_layout(s_raw_lbl);
    lv_obj_set_x(s_sma_lbl, MCH_X + 8 + lv_obj_get_width(s_raw_lbl) + 10);
    snprintf(txt, sizeof txt, "last %u s, cps", (unsigned)s_ms.interval_s);
    lv_label_set_text(s_last_cap, txt);
    if (s_ms.running) {
        snprintf(txt, sizeof txt, "recording, %d%s", s_iv_n, sd_log_mon_active() ? ", SD" : "");
        lv_label_set_text(s_mon_stat, txt);
    } else {
        lv_label_set_text(s_mon_stat, "stopped");
    }

    if (s_iv_n == 0) {
        lv_label_set_text(s_sess_val, "--");
        lv_label_set_text(s_sess_err, "");
        lv_label_set_text(s_last_val, "--");
        lv_label_set_text(s_last_err, "");
        lv_label_set_text(s_mon_msg, msg);
        lv_obj_clear_flag(s_mon_msg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_mxunit, LV_OBJ_FLAG_HIDDEN);
    } else {
        fmt_cps((double)s_sess_counts / (double)s_sess_dur, txt, sizeof txt);
        lv_label_set_text(s_sess_val, txt);
        snprintf(txt, sizeof txt, "+/-%.2f%%", 100.0 / sqrt((double)(s_sess_counts ? s_sess_counts : 1)));
        lv_label_set_text(s_sess_err, s_sess_counts ? txt : "");
        const mon_iv_t *last = &s_iv[s_iv_n - 1];
        fmt_cps((double)last->counts / last->dur, txt, sizeof txt);
        lv_label_set_text(s_last_val, txt);
        snprintf(txt, sizeof txt, "+/-%.2f%%", 100.0 / sqrt((double)(last->counts ? last->counts : 1)));
        lv_label_set_text(s_last_err, last->counts ? txt : "");

        int start;
        float mn, mx;
        int cnt = mon_series(win, &start, &mn, &mx);
#define MY_OF(v) ((MCH_H - 1) - (int)(((v) - mn) / (mx - mn) * (MCH_H - 1) + 0.5f))

        // Сетка Y — 5 линий на равных долях диапазона, как в вебе (не niceStep).
        for (int g = 0; g <= 4 && ny < N_MYLAB; g++) {
            float v = mn + (mx - mn) * g / 4;
            int y = MY_OF(v);
            for (int x = 0; x < MCH_W; x++) MPX(x, y) = s_c_mgrid;
            fmt_cps(v, txt, sizeof txt);
            lv_label_set_text(s_mylab[ny], txt);
            lv_obj_set_pos(s_mylab[ny], 0, MCH_Y + y - 7);
            lv_obj_clear_flag(s_mylab[ny], LV_OBJ_FLAG_HIDDEN);
            ny++;
        }

        // Ось X. С часами платы — настенное время конца интервала (ts веба);
        // без них — наборное время прибора от начала сессии.
        bool wall = monitor_wall_of(s_iv[start].up) != 0;
        int64_t t0, tn, sh = wall ? (int64_t)boot_config_tz_min() * 60 : 0;
        if (wall) {
            t0 = monitor_wall_of(s_iv[start].up);
            tn = monitor_wall_of(last->up);
        } else {
            t0 = (int64_t)s_iv[start].dev_end - s_sess_t0;
            tn = (int64_t)last->dev_end - s_sess_t0;
        }
        int64_t span = tn - t0;
#define MX_OF(t) ((int)((double)((t) - t0) / span * (MCH_W - 1) + 0.5))
        if (span > 0) {
            uint32_t step = x_tick_step((uint32_t)span);
            int64_t tick0 = ((t0 + sh + step - 1) / (int64_t)step) * step - sh;   // круглое время пояса
            for (int64_t tm = tick0; tm <= tn && nx < N_MXLAB; tm += step) {
                int x = MX_OF(tm);
                if (x < 0 || x >= MCH_W) continue;
                for (int y = 0; y < MCH_H; y++) MPX(x, y) = s_c_grid;
                if (!wall && x > MCH_W - XUNIT_W - MXLAB_W / 2) continue;   // не наезжать на подпись оси
                if (wall) fmt_wall(tm + sh, step, (uint32_t)span, txt, sizeof txt);
                else      fmt_elapsed((uint32_t)tm, step, txt, sizeof txt);
                int lx = MCH_X + x - MXLAB_W / 2;
                if (lx > TFT_H_RES - MXLAB_W) lx = TFT_H_RES - MXLAB_W;   // крайняя справа — в экран
                lv_label_set_text(s_mxlab[nx], txt);
                lv_obj_set_pos(s_mxlab[nx], lx, MXLAB_Y);
                lv_obj_clear_flag(s_mxlab[nx], LV_OBJ_FLAG_HIDDEN);
                nx++;
            }
        }

        for (int k = 0; k < cnt; k++) {
            const mon_iv_t *v = &s_iv[start + k];
            int x;
            if (span > 0) x = MX_OF(wall ? (int64_t)monitor_wall_of(v->up) : (int64_t)v->dev_end - s_sess_t0);
            else          x = cnt > 1 ? k * (MCH_W - 1) / (cnt - 1) : MCH_W / 2;
            pr[k].x = ps[k].x = (lv_coord_t)x;
            pr[k].y = (lv_coord_t)MY_OF(raw[k]);
            ps[k].y = (lv_coord_t)MY_OF(sma[k]);
        }
#undef MX_OF
#undef MY_OF
        // Сырая линия акцентом, поверх — SMA оранжевым (как drawChart()).
        lv_draw_line_dsc_t ld;
        lv_draw_line_dsc_init(&ld);
        ld.color = lv_color_hex(COL_ACCENT);
        ld.width = 2;   // #MON-3: как у SMA, без точек на вершинах (запрос 2026-09-29)
        if (cnt >= 2) lv_canvas_draw_line(s_mon_plot, pr, cnt, &ld);
        ld.color = lv_color_hex(COL_SMA);
        ld.width = 2;
        if (cnt >= 2) lv_canvas_draw_line(s_mon_plot, ps, cnt, &ld);

        if (wall) lv_obj_add_flag(s_mxunit, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_clear_flag(s_mxunit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_mon_msg, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = ny; i < N_MYLAB; i++) lv_obj_add_flag(s_mylab[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = nx; i < N_MXLAB; i++) lv_obj_add_flag(s_mxlab[i], LV_OBJ_FLAG_HIDDEN);
    draw_frame(s_mon_buf, MCH_W, MCH_H);
    lv_obj_invalidate(s_mon_plot);
}

// ---------------- вид «на трубе» (#MNT-1) ----------------

// #MNT-3: число CPS — самым крупным шрифтом, что помещается левее правой колонки
// (батарея / CPS / доза), низом к графику. 104 px — до 3 цифр, 80 — 4, 64 — 5.
// #MNT-4: размер прыгал — на ~1000 CPS число то 3, то 4 цифры, и шрифт менялся
// каждую секунду. Теперь ширина — худшая для этого числа цифр (все «4», самая
// широкая цифра), уменьшение — сразу, увеличение — если крупнее влезает 10 с подряд.
#define MNT_FONT_UP_MS  10000
static void mnt_layout_top(void)
{
    static const lv_font_t *const F[4] = { &font_cps104, &font_cps80, &font_cps64, &lv_font_montserrat_28 };
    lv_obj_update_layout(s_mnt);
    int col_x = MNT_PW;
    lv_obj_t *const col[4] = { s_mnt_dot, s_mnt_batt, s_mnt_unit, s_mnt_dose };
    for (int i = 0; i < 4; i++)
        if (!lv_obj_has_flag(col[i], LV_OBJ_FLAG_HIDDEN) && lv_obj_get_width(col[i]) > 0 &&
            lv_obj_get_x(col[i]) < col_x)
            col_x = lv_obj_get_x(col[i]);
    int maxw = col_x - 8 - 8;
    char worst[16];
    const char *txt = lv_label_get_text(s_mnt_cps);
    int n = 0;
    for (; txt[n] && n < (int)sizeof worst - 1; n++) worst[n] = (txt[n] >= '0' && txt[n] <= '9') ? '4' : txt[n];
    worst[n] = '\0';
    int fit = 3;
    for (int i = 0; i < 3; i++) {
        lv_point_t sz;
        lv_txt_get_size(&sz, worst, F[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= maxw) { fit = i; break; }
    }
    uint32_t now = lv_tick_get();
    if (fit > s_mnt_fi) {                 // не влезает — мельче сразу
        s_mnt_fi = fit;
        s_mnt_up_since = 0;
    } else if (fit < s_mnt_fi) {          // крупнее — только после паузы
        if (!s_mnt_up_since) s_mnt_up_since = now ? now : 1;
        else if (now - s_mnt_up_since >= MNT_FONT_UP_MS) { s_mnt_fi = fit; s_mnt_up_since = 0; }
    } else {
        s_mnt_up_since = 0;
    }
    const lv_font_t *f = F[s_mnt_fi];
    lv_obj_set_style_text_font(s_mnt_cps, f, 0);
    lv_obj_set_pos(s_mnt_cps, 8, MNT_TOP_H - lv_font_get_line_height(f));
}

// Крупное CPS и доза (если задан коэффициент, как в шапке). Под lvgl_port_lock.
static void mnt_render_cps(bool valid)
{
    char txt[24];
    if (valid) lv_label_set_text_fmt(s_mnt_cps, "%lu", (unsigned long)s_snap->cps);
    else       lv_label_set_text(s_mnt_cps, "--");
    float k = spectrogram_get_dose_k();
    if (k > 0 && valid) {
        snprintf(txt, sizeof txt, "%.3f uSv/h", (double)(s_snap->cps * k));
        lv_label_set_text(s_mnt_dose, txt);
        lv_obj_clear_flag(s_mnt_dose, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_mnt_dose, LV_OBJ_FLAG_HIDDEN);
    }
    mnt_layout_top();
}

// Батарея (как #batt-chip) и точка прибора у правого края. Под lvgl_port_lock.
static void mnt_render_status(const battery_status_t *bs)
{
    if (bs->present) {
        int pct = bs->percent < 0 ? 0 : bs->percent > 100 ? 100 : bs->percent;
        lv_label_set_text_fmt(s_mnt_batt, "%s%d%%", bs->charging ? LV_SYMBOL_CHARGE : "", pct);
        lv_obj_set_style_text_color(s_mnt_batt, lv_color_hex(pct < 20 ? COL_RED_TXT : pct < 50 ? COL_WARN : COL_CHIP_OK), 0);
    } else {
        lv_label_set_text(s_mnt_batt, "");
    }
    lv_obj_update_layout(s_mnt_batt);
    lv_obj_align_to(s_mnt_dot, s_mnt_batt, LV_ALIGN_OUT_LEFT_MID, bs->present ? -8 : 0, 0);
    mnt_layout_top();   // ширина батареи меняет место под число
}

#define MSPX(x, y) s_mnt_spec_buf[(y) * MNT_W + (x)]
#define MMPX(x, y) s_mnt_mon_buf[(y) * MNT_W + (x)]

// Спектр без подписей делений: сетка, заливка, линия — как render_spectrum(),
// всегда живой (не открытый файл). Подпись справа: окно и шкала. Под lvgl_port_lock.
static void mnt_render_spectrum(bool valid, const char *msg)
{
    char txt[40];
    for (int i = 0; i < MNT_W * MNT_SPEC_H; i++) s_mnt_spec_buf[i] = s_c_bg;
    if (valid) {
        const spectrum_data_t *sp = s_snap;
        float tt = sp->total_time_sec > 0 ? (float)sp->total_time_sec : 1.0f;
        float mx = build_columns(sp, tt, MNT_W);
        if (mx <= 0) mx = 1;
        bool lg = s_spec_log;
        float ylo, yhi;
        double t0, tstep, tend;
        spec_yscale(mx, tt, lg, &ylo, &yhi, &t0, &tstep, &tend);
        for (double t = t0; t <= tend; t += tstep) {
            int y = y_row_h((float)(lg ? pow(10, t) : t), lg, ylo, yhi, MNT_SPEC_H);
            for (int x = 0; x < MNT_W; x++) MSPX(x, y) = s_c_grid;
        }
        const int range = SPEC_CH_TO - SPEC_CH_FROM + 1;
        bool kev = sp->calib_valid;
        double lo = kev ? ch2kev(sp, SPEC_CH_FROM) : SPEC_CH_FROM;
        double hi = kev ? ch2kev(sp, SPEC_CH_TO + 1) : SPEC_CH_TO + 1;
        double step = nice_step(hi, 6);
        for (double k = 0; k <= hi * 1.01; k += step) {
            if (k < lo) continue;
            double ch = kev ? kev2ch(sp, k) : k;
            int x = (int)((ch - SPEC_CH_FROM) / range * MNT_W + 0.5);
            if (x < 0 || x >= MNT_W) continue;
            for (int y = 0; y < MNT_SPEC_H; y++) MSPX(x, y) = s_c_grid;
        }
        int prev = y_row_h(s_col[0], lg, ylo, yhi, MNT_SPEC_H);
        for (int x = 0; x < MNT_W; x++) {
            int y = y_row_h(s_col[x], lg, ylo, yhi, MNT_SPEC_H);
            if (y < MNT_SPEC_H - 1)
                for (int yy = y; yy < MNT_SPEC_H; yy++)
                    MSPX(x, yy) = MSPX(x, yy).full == s_c_grid.full ? s_c_fill_grid : s_c_fill;
            int a = prev < y ? prev : y, b = prev < y ? y : prev;
            for (int yy = a; yy <= b; yy++) MSPX(x, yy) = s_c_line;
            prev = y;
        }
        if (kev) snprintf(txt, sizeof txt, "%.0f-%.0f keV, %s", lo, hi, lg ? "log" : "lin");
        else     snprintf(txt, sizeof txt, "ch %d-%d, %s", SPEC_CH_FROM, SPEC_CH_TO, lg ? "log" : "lin");
        lv_label_set_text(s_mnt_sinfo, txt);
        lv_obj_clear_flag(s_mnt_sinfo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_mnt_smsg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_mnt_sinfo, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_mnt_smsg, msg ? msg : "");
        lv_obj_clear_flag(s_mnt_smsg, LV_OBJ_FLAG_HIDDEN);
    }
    draw_frame(s_mnt_spec_buf, MNT_W, MNT_SPEC_H);
    lv_obj_invalidate(s_mnt_spec);
}

// Монитор: та же сессия и тот же ряд, что вкладка Monitor (mon_series), точки —
// равномерно по ширине; подписи — верх и низ шкалы и легенда. Под lvgl_port_lock.
static void mnt_render_monitor(const char *msg)
{
    char txt[24];
    for (int i = 0; i < MNT_W * MNT_MON_H; i++) s_mnt_mon_buf[i] = s_c_bg;
    int win = s_ms.sma_win < MON_SMA_MIN ? MON_SMA_MIN : s_ms.sma_win;
    lv_label_set_text_fmt(s_mnt_sma, "SMA %d", win);
    lv_label_set_text_fmt(s_mnt_raw, "CPS %us", (unsigned)(s_ms.interval_s ? s_ms.interval_s : 1));
    lv_obj_update_layout(s_mnt_sma);
    lv_obj_align_to(s_mnt_raw, s_mnt_sma, LV_ALIGN_OUT_LEFT_MID, -10, 0);
    if (s_iv_n == 0) {
        lv_obj_add_flag(s_mnt_mhi, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_mnt_mlo, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_mnt_mmsg, msg);
        lv_obj_clear_flag(s_mnt_mmsg, LV_OBJ_FLAG_HIDDEN);
    } else {
        int start;
        float mn, mx;
        int cnt = mon_series(win, &start, &mn, &mx);
        (void)start;
#define MMY_OF(v) ((MNT_MON_H - 1) - (int)(((v) - mn) / (mx - mn) * (MNT_MON_H - 1) + 0.5f))
        for (int g = 0; g <= 4; g++) {
            int y = MMY_OF(mn + (mx - mn) * g / 4);
            for (int x = 0; x < MNT_W; x++) MMPX(x, y) = s_c_mgrid;
        }
        for (int k = 0; k < cnt; k++) {
            int x = cnt > 1 ? k * (MNT_W - 1) / (cnt - 1) : MNT_W / 2;
            s_m_pr[k].x = s_m_ps[k].x = (lv_coord_t)x;
            s_m_pr[k].y = (lv_coord_t)MMY_OF(s_m_raw[k]);
            s_m_ps[k].y = (lv_coord_t)MMY_OF(s_m_sma[k]);
        }
#undef MMY_OF
        lv_draw_line_dsc_t ld;
        lv_draw_line_dsc_init(&ld);
        ld.width = 2;
        ld.color = lv_color_hex(COL_ACCENT);
        if (cnt >= 2) lv_canvas_draw_line(s_mnt_mon, s_m_pr, cnt, &ld);
        ld.color = lv_color_hex(COL_SMA);
        if (cnt >= 2) lv_canvas_draw_line(s_mnt_mon, s_m_ps, cnt, &ld);
        fmt_cps(mx, txt, sizeof txt);
        lv_label_set_text(s_mnt_mhi, txt);
        fmt_cps(mn, txt, sizeof txt);
        lv_label_set_text(s_mnt_mlo, txt);
        lv_obj_clear_flag(s_mnt_mhi, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_mnt_mlo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_mnt_mmsg, LV_OBJ_FLAG_HIDDEN);
    }
    draw_frame(s_mnt_mon_buf, MNT_W, MNT_MON_H);
    lv_obj_invalidate(s_mnt_mon);
}

// Включить/выключить вид «на трубе»: портрет и слой. lv_disp_set_rotation()
// меняет размер экрана LVGL (480x320 <-> 320x480), перерисовывает всё и зовёт
// lvgl_port_update_callback() — тот выставляет MADCTL панели из .rotation
// (MV -> без MV, MX). Координаты тача LVGL поворачивает сам (lv_indev.c), в
// touch_process() они остаются альбомными. Под lvgl_port_lock — кадр не идёт;
// команду MADCTL esp_lcd отправляет, дождавшись переданных кусков кадра.
// Объекты обычного вида при портрете частично за краем — их закрывает s_mnt.
static void mount_set(bool on)
{
    s_mount = on;
    if (on) lv_obj_clear_flag(s_mnt, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(s_mnt, LV_OBJ_FLAG_HIDDEN);
    lv_disp_set_rotation(NULL, on ? MNT_ROT : LV_DISP_ROT_NONE);
}

static void mount_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("tft", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_u8(h, "mount", &v) == ESP_OK) s_mount = v != 0;
    nvs_close(h);
}

static void mount_save(void)   // tft_task: NVS — не из задачи LVGL (стек в PSRAM, #RAM-1)
{
    nvs_handle_t h;
    if (nvs_open("tft", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "mount", s_mount ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---------------- водопад (#TFT-28) ----------------

// Inferno — палитра по умолчанию веба (PALETTES.Inferno), LUT на 256 как buildLut().
static void wf_build_lut(void)
{
    static const uint8_t ST[10][3] = {
        {0, 0, 4}, {20, 11, 52}, {58, 9, 99}, {101, 21, 110}, {143, 36, 108},
        {185, 53, 95}, {222, 73, 74}, {247, 104, 52}, {252, 143, 30}, {252, 194, 72},
    };
    for (int i = 0; i < 256; i++) {
        float f = i / 255.0f * 9;
        int si = (int)f;
        float g = f - si;
        if (si >= 9) { si = 8; g = 1; }
        uint8_t c[3];
        for (int k = 0; k < 3; k++) c[k] = (uint8_t)(ST[si][k] + (ST[si + 1][k] - ST[si][k]) * g);
        s_wf_lut[i] = lv_color_make(c[0], c[1], c[2]);
    }
}

static void wf_reset(void)
{
    s_wf_n = s_wf_head = 0;
    s_wf_norm = 64;   // clearRows() веба
}

// Строка платы (8192 канала) -> колонки экрана по максимуму, в кольцо.
static bool wf_push_row(void *ctx, const uint16_t *row, size_t bytes)
{
    (void)ctx; (void)bytes;
    const int range = SPEC_CH_TO - SPEC_CH_FROM + 1;
    uint16_t *dst = s_wf_cols + (size_t)s_wf_head * PLOT_W, rm = 0;
    int ch0 = SPEC_CH_FROM;
    for (int x = 0; x < PLOT_W; x++) {
        int ch1 = SPEC_CH_FROM + (int)((int64_t)range * (x + 1) / PLOT_W);
        if (ch1 <= ch0) ch1 = ch0 + 1;
        uint16_t m = 0;
        for (int ch = ch0; ch < ch1; ch++) if (row[ch] > m) m = row[ch];
        dst[x] = m;
        if (m > rm) rm = m;
        ch0 = ch1;
    }
    s_wf_rmax[s_wf_head] = rm;
    s_wf_head = (s_wf_head + 1) % PLOT_H;
    if (s_wf_n < PLOT_H) s_wf_n++;
    return true;
}

// Новые строки с платы. Отставание больше WF_FETCH_MAX, очистка, первый вход —
// перечитать окно целиком (последние PLOT_H строк кольца). true — есть новое.
static bool wf_pull(void)
{
    spectrogram_get_status(&s_wf_st);
    uint32_t tot = s_wf_st.total_rows;
    if (s_wf_synced && tot == s_wf_seen) return false;
    if (!s_wf_synced || tot < s_wf_seen || tot - s_wf_seen > WF_FETCH_MAX) {
        wf_reset();
        uint32_t first = 0;
        size_t n = spectrogram_stream_window(s_wf_rowbuf, PLOT_H, &first, wf_push_row, NULL);
        s_wf_seen = first + (uint32_t)n;
        s_wf_synced = true;
        return true;
    }
    uint32_t first = 0;
    size_t n = spectrogram_copy_window(s_wf_rowbuf, tot - s_wf_seen, &first);
    for (size_t i = 0; i < n; i++)
        if (first + i >= s_wf_seen) wf_push_row(NULL, s_wf_rowbuf + i * WF_CHANNELS, WF_ROW_BYTES);
    if (n) s_wf_seen = first + (uint32_t)n;
    return n > 0;
}

// Шаг подписей времени слева: круглый, не чаще ~30 px.
static uint32_t wf_time_step(uint32_t iv)
{
    static const uint32_t M[] = {10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 21600, 43200, 86400};
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++)
        if (M[i] / iv >= 30) return M[i];
    return M[sizeof M / sizeof M[0] - 1];
}

// draw() веба: log1p(v*gfac)/log1p(normMax), gain 40 -> gfac 1, normMax плавает
// 0.7/0.3 к максимуму видимых строк. Пустые строки — rgb(8,10,8). Под lvgl_port_lock.
static void wf_render(void)
{
    char txt[64];   // #SD-5: статус с «NO SD - not saved»
    int ny = 0, nx = 0;
    uint16_t seen = 1;
    for (int k = 0; k < s_wf_n; k++) {
        int r = (s_wf_head - 1 - k + PLOT_H) % PLOT_H;
        if (s_wf_rmax[r] > seen) seen = s_wf_rmax[r];
    }
    s_wf_norm = s_wf_norm * 0.7f + seen * 0.3f;
    if (s_wf_norm < 1) s_wf_norm = 1;
    const float inv = 1.0f / log1pf(s_wf_norm);
    lv_color_t empty = lv_color_make(8, 10, 8);
    for (int y = 0; y < PLOT_H; y++) {
        lv_color_t *out = s_wf_buf + (size_t)y * PLOT_W;
        if (y >= s_wf_n) {
            for (int x = 0; x < PLOT_W; x++) out[x] = empty;
            continue;
        }
        const uint16_t *src = s_wf_cols + (size_t)((s_wf_head - 1 - y + PLOT_H) % PLOT_H) * PLOT_W;
        for (int x = 0; x < PLOT_W; x++) {
            float t = src[x] ? log1pf(src[x]) * inv : 0;
            int i = (int)(t * 256);
            out[x] = s_wf_lut[i < 0 ? 0 : i > 255 ? 255 : i];
        }
    }
    lv_obj_invalidate(s_wf_plot);

    // Ось X — как у спектра: кэВ по калибровке прибора, без неё — каналы.
    const spectrum_data_t *sp = s_snap;
    const int range = SPEC_CH_TO - SPEC_CH_FROM + 1;
    bool kev = sp->valid && sp->calib_valid;
    double lo = kev ? ch2kev(sp, SPEC_CH_FROM) : SPEC_CH_FROM;
    double hi = kev ? ch2kev(sp, SPEC_CH_TO + 1) : SPEC_CH_TO + 1;
    double step = nice_step(hi, 6);
    for (double k = 0; k <= hi * 1.01 && nx < N_XLAB; k += step) {
        if (k < lo) continue;
        double ch = kev ? kev2ch(sp, k) : k;
        int x = (int)((ch - SPEC_CH_FROM) / range * PLOT_W + 0.5);
        if (x < 0 || x >= PLOT_W || x > PLOT_W - XUNIT_W - 14) continue;
        snprintf(txt, sizeof txt, "%.0f", k);
        lv_label_set_text(s_wf_xlab[nx], txt);
        lv_obj_set_pos(s_wf_xlab[nx], PLOT_X + x - 20, XLAB_Y);
        lv_obj_clear_flag(s_wf_xlab[nx], LV_OBJ_FLAG_HIDDEN);
        nx++;
    }
    lv_label_set_text(s_wf_xunit, kev ? "keV" : "Channel");
    lv_obj_clear_flag(s_wf_xunit, LV_OBJ_FLAG_HIDDEN);

    // Слева — сколько назад (строка = интервал записи).
    uint32_t iv = s_wf_st.interval_sec ? s_wf_st.interval_sec : WF_INTERVAL_DEFAULT;
    uint32_t ts = wf_time_step(iv);
    for (uint32_t sec = ts; ny < N_WYLAB; sec += ts) {
        int y = (int)(sec / iv);
        if (y >= PLOT_H - 6) break;
        if (sec % 3600 == 0)    snprintf(txt, sizeof txt, "-%luh", (unsigned long)(sec / 3600));
        else if (sec % 60 == 0) snprintf(txt, sizeof txt, "-%lum", (unsigned long)(sec / 60));
        else                    snprintf(txt, sizeof txt, "-%lus", (unsigned long)sec);
        lv_label_set_text(s_wf_ylab[ny], txt);
        lv_obj_set_pos(s_wf_ylab[ny], 0, PLOT_Y + y - 7);
        lv_obj_clear_flag(s_wf_ylab[ny], LV_OBJ_FLAG_HIDDEN);
        ny++;
    }
    for (int i = nx; i < N_XLAB; i++) lv_obj_add_flag(s_wf_xlab[i], LV_OBJ_FLAG_HIDDEN);
    for (int i = ny; i < N_WYLAB; i++) lv_obj_add_flag(s_wf_ylab[i], LV_OBJ_FLAG_HIDDEN);

    // Статус (#hud / индикатор записи веба) и причина пустого экрана.
    snprintf(txt, sizeof txt, "%s  %lu s  rows %lu%s", s_wf_st.recording ? "REC" : "stopped",
             (unsigned long)iv, (unsigned long)s_wf_st.total_rows,
             s_wf_st.persist && !s_wf_st.sd ? "  NO SD - not saved" : "");   // #SD-5
    lv_label_set_text(s_wf_stat, txt);
    lv_obj_set_style_text_color(s_wf_stat, lv_color_hex(s_wf_st.recording ? COL_RED_TXT : COL_DIM), 0);
    if (s_wf_n == 0) {
        lv_label_set_text(s_wf_msg, s_wf_st.recording ? "Waiting for the first row"
                                                      : "Waterfall not recording - press Start");
        lv_obj_clear_flag(s_wf_msg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_wf_msg, LV_OBJ_FLAG_HIDDEN);
    }
}

// Start/Stop на вкладке Waterfall — как кнопки страницы водопада веба
// (/api/waterfall/start шлёт прибору -sta, чтобы строки не были пустыми).
static void wf_cmd(uint8_t cmd)
{
    char msg[48];
    uint32_t col = COL_ACCENT;
    if (cmd == TFT_CMD_START) {
        if (spectrogram_start() == 0) {
            int tx = usb_host_send_text_command("-sta");
            if (tx == 0) acq_hint(true);   // #TFT-37
            snprintf(msg, sizeof msg, tx == 0 ? "Waterfall recording" : "Waterfall recording, no spectrometer");
            if (tx != 0) col = COL_WARN;
        } else {
            snprintf(msg, sizeof msg, "Waterfall start failed");
            col = COL_RED_TXT;
        }
    } else {
        spectrogram_stop();
        snprintf(msg, sizeof msg, "Waterfall stopped");
    }
    ESP_LOGI(TAG, "кнопка %s: водопад", cmd == TFT_CMD_START ? "Start" : "Stop");
    lvgl_port_lock(0);
    toast_show(msg, col);
    lvgl_port_unlock();
}

// ---------------- Files (#TFT-34) ----------------

static const char *files_path(int d)
{
    return d == 0 ? spectrum_saved_dir() : d == 1 ? SD_MOUNT_POINT "/monitor" : SD_MOUNT_POINT "/wf";
}

// Локальные дата-время (пояс из веба) для подписей.
static void fmt_local(time_t utc, char *out, size_t n)
{
    if (utc < 1700000000) { snprintf(out, n, "time not set"); return; }
    time_t t = utc + (time_t)boot_config_tz_min() * 60;
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d", (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100,
             tm.tm_mday % 100, tm.tm_hour % 100, tm.tm_min % 100);
}

// Прочитать каталог: FILES_MAX новейших (спектры — по номеру, остальные — по
// имени, в именах дата). Только tft_task.
static void files_scan(int d)
{
    const char *path = files_path(d);
    s_fent_n = s_fent_more = 0;
    DIR *dir = opendir(path);
    struct dirent *de;
    while (dir && (de = readdir(dir)) != NULL) {
        if (de->d_type == DT_DIR) continue;
        fent_t e = { .idx = -1 };
        if (d == 0 && (sscanf(de->d_name, "spec_%d.bin", &e.idx) != 1 || e.idx < 0)) continue;
        strlcpy(e.name, de->d_name, sizeof e.name);
        int pos = s_fent_n;   // вставка с сортировкой по убыванию
        while (pos > 0 && (d == 0 ? s_fent[pos - 1].idx < e.idx : strcmp(s_fent[pos - 1].name, e.name) < 0)) pos--;
        if (pos >= FILES_MAX) { s_fent_more++; continue; }
        if (s_fent_n == FILES_MAX) { s_fent_n--; s_fent_more++; }
        memmove(&s_fent[pos + 1], &s_fent[pos], (size_t)(s_fent_n - pos) * sizeof(fent_t));
        s_fent[pos] = e;
        s_fent_n++;
    }
    if (dir) closedir(dir);
    char p[300];
    for (int i = 0; i < s_fent_n; i++) {
        fent_t *e = &s_fent[i];
        snprintf(p, sizeof p, "%s/%s", path, e->name);
        if (d == 0) {   // заголовок spectrum_data_t — как handle_list() веба
            FILE *f = fopen(p, "rb");
            if (!f) continue;
            fseek(f, offsetof(spectrum_data_t, total_counts), SEEK_SET);
            fread(&e->counts, 4, 1, f);
            fread(&e->time_s, 4, 1, f);
            fseek(f, offsetof(spectrum_data_t, saved_at), SEEK_SET);
            fread(&e->saved, sizeof(time_t), 1, f);
            fclose(f);
        } else {
            struct stat st;
            if (stat(p, &st) == 0) e->size = (uint32_t)st.st_size;
        }
    }
}

static void files_show(int d)   // под lvgl_port_lock
{
    char a[80], b[48], g[16];
    int rows = s_fent_n + (s_fent_more ? 1 : 0);
    lv_table_set_row_cnt(s_files_tbl, rows ? rows : 1);
    for (int i = 0; i < s_fent_n; i++) {
        const fent_t *e = &s_fent[i];
        if (d == 0) {
            fmt_local(e->saved, b, sizeof b);
            snprintf(a, sizeof a, "#%04d   %s", e->idx, b);
            fmt_group(e->counts, g, sizeof g);
            snprintf(b, sizeof b, "%s cnt, %lu s", g, (unsigned long)e->time_s);
        } else {
            snprintf(a, sizeof a, "%s", e->name);
            if (e->size >= 1024 * 1024) snprintf(b, sizeof b, "%.1f MB", e->size / 1048576.0);
            else                        snprintf(b, sizeof b, "%.1f KB", e->size / 1024.0);
        }
        lv_table_set_cell_value(s_files_tbl, i, 0, a);
        lv_table_set_cell_value(s_files_tbl, i, 1, b);
        lv_table_add_cell_ctrl(s_files_tbl, i, 0, LV_TABLE_CELL_CTRL_TEXT_CROP);   // длинное имя — в одну строку
        lv_table_add_cell_ctrl(s_files_tbl, i, 1, LV_TABLE_CELL_CTRL_TEXT_CROP);
    }
    if (s_fent_more) {
        snprintf(a, sizeof a, "... and %d older, not shown", s_fent_more);
        lv_table_set_cell_value(s_files_tbl, s_fent_n, 0, a);
        lv_table_set_cell_value(s_files_tbl, s_fent_n, 1, "");
    }
    if (!rows) {
        lv_table_set_cell_value(s_files_tbl, 0, 0, d && !sd_card_present() ? "No SD card" : "No files");
        lv_table_set_cell_value(s_files_tbl, 0, 1, "");
    }
    for (int i = 0; i < 3; i++)
        btn_select(s_files_dirbtn[i], i == d);   // #TFT-36
    lv_obj_scroll_to_y(s_files_tbl, 0, LV_ANIM_OFF);
    lv_obj_clear_flag(s_files, LV_OBJ_FLAG_HIDDEN);
}

// Касание строки: сохранённый спектр — открыть на графике; прочее — только список.
static void files_open_sel(void)
{
    int r = s_files_sel, d = s_files_dir;
    if (r < 0 || r >= s_fent_n) return;
    char msg[64];
    uint32_t col = COL_DIM;
    bool opened = false;
    if (d == 0) {
        if (spectrum_load_from_flash(s_fent[r].idx, s_saved) == 0) {
            opened = true;
        } else {
            snprintf(msg, sizeof msg, "Cannot read spectrum #%d", s_fent[r].idx);
            col = COL_RED_TXT;
        }
    } else {
        snprintf(msg, sizeof msg, "%.40s: view on PC", s_fent[r].name);
    }
    lvgl_port_lock(0);
    if (opened) {
        char when[24], g[16], txt[96];
        fmt_local(s_saved->saved_at, when, sizeof when);
        fmt_group(s_saved->total_counts, g, sizeof g);
        snprintf(txt, sizeof txt, "SAVED #%04d  %s  %lu s  %s cnt", s_fent[r].idx, when,
                 (unsigned long)s_saved->total_time_sec, g);
        lv_label_set_text(s_saved_lbl, txt);
        lv_obj_clear_flag(s_saved_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_live_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_files, LV_OBJ_FLAG_HIDDEN);
        s_saved_on = true;
        s_force_plot = true;
        view_set(0);
    } else {
        toast_show(msg, col);
    }
    lvgl_port_unlock();
}

// ---------------- Conf (#TFT-29) ----------------

static void conf_render(void)   // под lvgl_port_lock
{
    net_run_mode_t m = wifi_manager_mode();
    const char *now = m == NET_MODE_FIELD_AP
        ? (wifi_manager_ap_forced() ? "Now: Field mode (Outdoor AP)" : "Now: Field AP (fallback - home network not found)")
        : m == NET_MODE_SETUP ? "Now: setup portal (WiFi not configured)" : "Now: Home network (Indoor)";
    char ssid[33], ssid_a[40];
    int rssi = 0;
    if (wifi_manager_sta_info(ssid, sizeof ssid, &rssi)) {   // #TFT-41: текущая сеть
        ascii_copy(ssid_a, sizeof ssid_a, ssid);
        lv_label_set_text_fmt(s_net_lbl, "%s - %s, %d dBm", now, ssid_a, rssi);
    } else {
        lv_label_set_text(s_net_lbl, now);
    }
    int cur = m == NET_MODE_FIELD_AP ? 1 : m == NET_MODE_STA ? 0 : -1;
    for (int i = 0; i < 2; i++)
        btn_select(s_net_btn[i], i == cur);   // #TFT-36
    lv_slider_set_value(s_chw_slider, s_ch_to, LV_ANIM_OFF);
    lv_slider_set_left_value(s_chw_slider, s_ch_from, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_chw_lbl, "%d - %d", s_ch_from, s_ch_to);
}

static void chwin_load(void)
{
    nvs_handle_t h;
    uint32_t v;
    if (nvs_open("tft", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_u32(h, "chwin", &v) == ESP_OK) {
        int f = (int)(v >> 16), t = (int)(v & 0xFFFF);
        if (f >= 0 && t <= CH_WIN_MAX && t - f >= CH_WIN_MIN_SPAN) { s_ch_from = f; s_ch_to = t; }
    }
    nvs_close(h);
}

static void chwin_apply(void)   // tft_task: единственный писатель окна
{
    int f = s_chw_pend_from, t = s_chw_pend_to;
    if (f < 0 || t > CH_WIN_MAX || t - f < CH_WIN_MIN_SPAN) return;
    if (f == s_ch_from && t == s_ch_to) return;
    s_ch_from = f;
    s_ch_to = t;
    s_wf_synced = false;   // строки водопада сжаты под старое окно — перечитать
    nvs_handle_t h;
    if (nvs_open("tft", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "chwin", ((uint32_t)f << 16) | (uint32_t)t);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "окно каналов экрана: %d..%d", f, t);
}

// ---------------- калибровка тача (#TFT-21): расчёт и хранение ----------------

// Решение 3x3 методом Гаусса с выбором главного элемента. m — расширенная матрица.
static bool solve3(double m[3][4], double out[3])
{
    for (int c = 0; c < 3; c++) {
        int p = c;
        for (int r = c + 1; r < 3; r++) if (fabs(m[r][c]) > fabs(m[p][c])) p = r;
        if (fabs(m[p][c]) < 1e-9) return false;          // точки на одной прямой
        for (int k = 0; k < 4; k++) { double t = m[c][k]; m[c][k] = m[p][k]; m[p][k] = t; }
        for (int r = 0; r < 3; r++) {
            if (r == c) continue;
            double f = m[r][c] / m[c][c];
            for (int k = c; k < 4; k++) m[r][k] -= f * m[c][k];
        }
    }
    for (int i = 0; i < 3; i++) out[i] = m[i][3] / m[i][i];
    return true;
}

// Наименьшие квадраты по TCAL_N точкам: s = k0*rx + k1*ry + k2, отдельно для
// sx и sy. Возвращает максимальную невязку в пикселях (< 0 — не решается).
static float cal_fit(touch_cal_t *out)
{
    double mx[3][4] = {{0}}, my[3][4] = {{0}}, kx[3], ky[3];
    for (int i = 0; i < TCAL_N; i++) {
        double v[3] = { s_cal_raw[i][0], s_cal_raw[i][1], 1.0 };
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) { mx[r][c] += v[r] * v[c]; my[r][c] += v[r] * v[c]; }
            mx[r][3] += v[r] * CAL_PTS[i].x;
            my[r][3] += v[r] * CAL_PTS[i].y;
        }
    }
    if (!solve3(mx, kx) || !solve3(my, ky)) return -1;
    *out = (touch_cal_t){ .magic = TCAL_MAGIC,
                          .a = (float)kx[0], .b = (float)kx[1], .c = (float)kx[2],
                          .d = (float)ky[0], .e = (float)ky[1], .f = (float)ky[2] };
    float worst = 0;
    for (int i = 0; i < TCAL_N; i++) {
        float ex = out->a * s_cal_raw[i][0] + out->b * s_cal_raw[i][1] + out->c - CAL_PTS[i].x;
        float ey = out->d * s_cal_raw[i][0] + out->e * s_cal_raw[i][1] + out->f - CAL_PTS[i].y;
        worst = fmaxf(worst, sqrtf(ex * ex + ey * ey));
    }
    return worst;
}

static void cal_load(void)
{
    nvs_handle_t h;
    size_t n = sizeof(s_tcal);
    esp_err_t err = nvs_open("tft", NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_blob(h, "tcal", &s_tcal, &n);
        nvs_close(h);
    }
    if (err != ESP_OK || n != sizeof(s_tcal) || s_tcal.magic != TCAL_MAGIC) {
        memset(&s_tcal, 0, sizeof(s_tcal));
        ESP_LOGW(TAG, "тач не откалиброван — калибровка: веб, «Система» -> «Откалибровать дисплей»");
        return;
    }
    ESP_LOGI(TAG, "калибровка тача из NVS: x=%.4f*rx%+.4f*ry%+.1f  y=%.4f*rx%+.4f*ry%+.1f",
             (double)s_tcal.a, (double)s_tcal.b, (double)s_tcal.c,
             (double)s_tcal.d, (double)s_tcal.e, (double)s_tcal.f);
}

static esp_err_t cal_save(const touch_cal_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("tft", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, "tcal", c, sizeof(*c));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void cal_start(void)   // под lvgl_port_lock
{
    // #MNT-1: крестики и калибровка — в обычном (альбомном) виде.
    if (s_mount) { mount_set(false); mount_save(); }
    s_cal_idx = 0;
    s_cal_active = true;
    lv_obj_clear_flag(s_cal_layer, LV_OBJ_FLAG_HIDDEN);
    cal_show_target(NULL);
    ESP_LOGI(TAG, "калибровка тача: старт");
}

static void toast_show(const char *txt, uint32_t col);

// Все точки собраны: расчёт, проверка невязки, запись. Под lvgl_port_lock.
static void cal_finish(void)
{
    touch_cal_t c;
    float err_px = cal_fit(&c);
    if (err_px < 0 || err_px > TCAL_MAX_ERR_PX) {
        // Промах мимо крестика или точки легли на прямую — заново, старая остаётся.
        char note[64];
        snprintf(note, sizeof note, "Inaccurate (%.0f px). Again, from 1.\n", (double)(err_px < 0 ? 999 : err_px));
        ESP_LOGW(TAG, "калибровка тача: невязка %.1f px > %d — повтор", (double)err_px, TCAL_MAX_ERR_PX);
        s_cal_idx = 0;
        s_cal_active = true;
        cal_show_target(note);
        return;
    }
    esp_err_t e = cal_save(&c);
    s_tcal = c;   // применяется сразу, даже если NVS отказал (тогда — до перезагрузки)
    lv_obj_add_flag(s_cal_layer, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "калибровка тача: невязка %.1f px, x=%.4f*rx%+.4f*ry%+.1f  y=%.4f*rx%+.4f*ry%+.1f, NVS: %s",
             (double)err_px, (double)c.a, (double)c.b, (double)c.c, (double)c.d, (double)c.e, (double)c.f,
             esp_err_to_name(e));
    if (e == ESP_OK) toast_show("Touch calibrated", COL_ACCENT);
    else             toast_show("Calibrated, but NOT saved (NVS)", COL_RED_TXT);
}

static void cal_abort(void)   // таймаут, под lvgl_port_lock
{
    s_cal_active = false;
    lv_obj_add_flag(s_cal_layer, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGW(TAG, "калибровка тача: нет касания %d с — отмена, прежняя калибровка остаётся",
             TCAL_TIMEOUT_MS / 1000);
    toast_show("Calibration cancelled", COL_RED_TXT);
}

bool tft_display_present(void) { return s_ui_ok; }

// #SD-2: см. tft_display.h. Замок порта рекурсивный — вложенный захват из
// задачи, которая уже держит его (tft_task), безопасен.
void tft_display_spi_lock(void)   { if (s_lvgl_up) lvgl_port_lock(0); }
void tft_display_spi_unlock(void) { if (s_lvgl_up) lvgl_port_unlock(); }
bool tft_display_touch_calibrated(void) { return s_tcal.magic == TCAL_MAGIC; }

bool tft_display_start_touch_cal(void)   // из веб-сервера: только очередь, LVGL не трогаем
{
    if (!s_ui_ok || !s_touch) return false;
    uint8_t cmd = TFT_CMD_CAL;
    return xQueueSend(s_cmd_q, &cmd, 0) == pdTRUE;
}

// ---------------- задача экрана ----------------

// Стек во внутренней RAM, а её впритык (#TFT-17). 6 КБ, а не 4: рисование линий
// монитора средствами LVGL идёт в этой задаче. Размер — по замеру в логе.
// #TFT-34: +1 КБ под чтение каталога и открытие файла с карты (было 6144,
// свободно оставалось 3.3 КБ до появления экрана Files).
#define TFT_TASK_STACK  7168
#define VIEW_MNT        9       // #MNT-1: «вкладка» вида «на трубе» в tft_task

static void toast_show(const char *txt, uint32_t col)   // под lvgl_port_lock
{
    lv_label_set_text(s_toast, txt);
    lv_obj_set_style_text_color(s_toast, lv_color_hex(col), 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_until = xTaskGetTickCount() + pdMS_TO_TICKS(2500);
    if (!s_toast_until) s_toast_until = 1;   // 0 занят под «не показано»
}

// #TFT-37: прибор подтверждает Start/Stop только ростом времени набора (свип раз
// в 1 с) — до тех пор кнопка и точка R верят своей команде. Только tft_task.
static int        s_acq_hint;         // 1 — послали -sta, -1 — -sto, 0 — нет
static TickType_t s_acq_hint_until;

static void acq_hint(bool start)
{
    s_acq_hint = start ? 1 : -1;
    s_acq_hint_until = xTaskGetTickCount() + pdMS_TO_TICKS(ACQ_HINT_MS);
}

static void send_device_cmd(uint8_t cmd)
{
    bool start = cmd == TFT_CMD_START;
    const char *c = start ? "-sta" : "-sto";
    int r = usb_host_send_text_command(c);   // те же байты, что POST /api/command из веба
    if (r == 0) acq_hint(start);
    ESP_LOGI(TAG, "кнопка %s: %s -> %s", start ? "Start" : "Stop", c,
             r == 0 ? "отправлено" : "НЕ отправлено (прибор не подключён?)");
    lvgl_port_lock(0);
    if (r == 0) toast_show(start ? "Start sent" : "Stop sent", COL_ACCENT);
    else        toast_show("No spectrometer: command not sent", COL_RED_TXT);
    lvgl_port_unlock();
}

// #TFT-27: Clear (после подтверждения) и Save — то же, что POST /api/reset и
// /api/save веба. На Monitor Clear — очистка сессии мониторинга.
static void action_cmd(uint8_t cmd)
{
    char msg[48];
    uint32_t col = COL_ACCENT;
    bool spec = cmd == TFT_CMD_MNT_CLEAR;   // #MNT-3: свайп — всегда спектр, вкладка под слоем не важна
    if (spec) cmd = TFT_CMD_CLEAR;
    if (cmd == TFT_CMD_CLEAR && s_view == 1 && !spec) {
        monitor_session_clear();
        snprintf(msg, sizeof msg, "Monitoring session cleared");
    } else if (cmd == TFT_CMD_CLEAR && s_view == 2 && !spec) {
        // Как /api/waterfall/clear: во время записи плата стирать не даёт.
        if (spectrogram_clear() == 0) snprintf(msg, sizeof msg, "Waterfall cleared");
        else { snprintf(msg, sizeof msg, "Stop waterfall recording first"); col = COL_WARN; }
    } else if (cmd == TFT_CMD_CLEAR) {
        int r = usb_host_send_text_command("-rst");
        spectrum_reset();
        ESP_LOGI(TAG, "кнопка Clear: -rst -> %s", r == 0 ? "отправлено" : "НЕ отправлено");
        if (r == 0) snprintf(msg, sizeof msg, "Spectrum cleared");
        else { snprintf(msg, sizeof msg, "Cleared on board, spectrometer not connected"); col = COL_WARN; }
    } else {
        int idx = spectrum_save_to_flash();   // запись во флеш — здесь, не в задаче LVGL
        ESP_LOGI(TAG, "кнопка Save: %d", idx);
        if (idx >= 0) {
            // #SD-7: CSV-копии больше нет — сохранение одним файлом spec_NNNN.bin.
            snprintf(msg, sizeof msg, "Saved #%d %s", idx, spectrum_saved_on_sd() ? "to SD" : "to flash (no SD)");
        } else {
            snprintf(msg, sizeof msg, "Not saved: %s",
                     idx == -1 ? "no spectrum" : idx == -2 ? "no space" : idx == -4 ? "no SD card" : "file system error");
            col = COL_RED_TXT;
        }
    }
    lvgl_port_lock(0);
    toast_show(msg, col);
    lvgl_port_unlock();
}

// #MON-2: Start/Stop на вкладке Monitor — общая сессия мониторинга платы.
static void monitor_cmd(uint8_t cmd)
{
    bool start = cmd == TFT_CMD_START;
    if (start) monitor_session_start(0);   // интервал — прежний интервал сессии
    else       monitor_session_stop();
    ESP_LOGI(TAG, "кнопка %s: сессия мониторинга", start ? "Start" : "Stop");
    lvgl_port_lock(0);
    toast_show(start ? "Monitoring started" : "Monitoring stopped", COL_ACCENT);
    lvgl_port_unlock();
}

static void tft_task(void *arg)
{
    (void)arg;
    // Причины отсутствия данных — те же, что показывает баннер веба (#FW-43).
    static const char *const MSG[4] = {
        NULL, "No spectrometer connected", "Spectrometer not powered", "Waiting for data",
    };
    char mon_wait[48];
    int64_t last_wall0 = -1;
    int last_tz = INT32_MIN;
    uint32_t last_total = 0, last_time = 0, last_cps = 0;
    int last_state = -1, last_view = -1, last_stat = -1;
    int last_acq = -1, last_run = -1;   // #TFT-37
    TickType_t t_adv = 0;               // когда время набора последний раз менялось (0 — не менялось)
    bool adv_seen = false;
    char last_cpu[24] = "";   // #SYS-1
    TickType_t last_plot = 0;   // #TFT-32
    char last_ip[16] = "-";
    UBaseType_t min_hwm = UINT32_MAX;

    while (1) {
        uint8_t cmd = 0;
        if (s_wake_set) {   // #TFT-32: команда или новый свип; 1.5 с — прибор молчит
            QueueSetMemberHandle_t m = xQueueSelectFromSet(s_wake_set, pdMS_TO_TICKS(1500));
            if (m == s_cmd_q)             xQueueReceive(s_cmd_q, &cmd, 0);
            else if (m == s_commit_sig) {
                xSemaphoreTake(s_commit_sig, 0);
                // #TFT-33: monitor_task будит тот же коммит (ядро 1) — дать ему
                // положить секундный сэмпл, иначе точка монитора видна свипом позже.
                vTaskDelay(pdMS_TO_TICKS(30));
            }
        } else {
            xQueueReceive(s_cmd_q, &cmd, pdMS_TO_TICKS(1000));   // команда или раз в секунду
        }
#if TFT_PERF
        static int64_t perf_last;
        int64_t perf_t0 = esp_timer_get_time();
#endif
        if (cmd == TFT_CMD_START || cmd == TFT_CMD_STOP) {
            if (s_view == 1)      monitor_cmd(cmd);
            else if (s_view == 2) wf_cmd(cmd);
            else                  send_device_cmd(cmd);
        }
        if (cmd == TFT_CMD_CLEAR || cmd == TFT_CMD_SAVE || cmd == TFT_CMD_MNT_CLEAR) action_cmd(cmd);
        if (cmd == TFT_CMD_CHWIN) chwin_apply();   // #TFT-29
        if (cmd == TFT_CMD_FILES) {               // #TFT-34: каталог читаем здесь, рисуем под замком
            int d = s_files_dir;
            files_scan(d);
            lvgl_port_lock(0);
            files_show(d);
            lvgl_port_unlock();
        }
        if (cmd == TFT_CMD_FILE_OPEN) files_open_sel();
        if (cmd == TFT_CMD_LIVE) {
            s_saved_on = false;
            s_force_plot = true;
            lvgl_port_lock(0);
            lv_obj_add_flag(s_saved_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_live_btn, LV_OBJ_FLAG_HIDDEN);
            lvgl_port_unlock();
        }
        if (cmd == TFT_CMD_ALARM_SAVE) {          // #ALM-1: пороги с экрана -> NVS
            int r = cps_alarm_set_config(&s_alm_pend);
            lvgl_port_lock(0);
            toast_show(r == 0 ? (s_alm_pend.enabled ? "Alarm saved: ON" : "Alarm saved: OFF")
                              : r == -1 ? "Thresholds must ascend" : "Applied, NOT saved (NVS)",
                       r == 0 ? COL_ACCENT : COL_RED_TXT);
            lvgl_port_unlock();
        }
        if (cmd == TFT_CMD_MNT_MON_CLEAR) {       // #MNT-5: как Clear на вкладке Monitor
            monitor_session_clear();
            ESP_LOGI(TAG, "свайп: сессия мониторинга очищена");
            lvgl_port_lock(0);
            toast_show("Monitoring cleared", COL_ACCENT);
            lvgl_port_unlock();
        }
        if (cmd == TFT_CMD_MON_START) {           // #MNT-2: как Start на вкладке Monitor
            if (!s_ms.running) {
                monitor_cmd(TFT_CMD_START);
            } else {
                lvgl_port_lock(0);
                toast_show("Monitoring is running", COL_DIM);
                lvgl_port_unlock();
            }
        }
        if (cmd == TFT_CMD_MOUNT) {               // #MNT-1: долгое нажатие в центре
            lvgl_port_lock(0);
            mount_set(!s_mount);
            lvgl_port_unlock();
            mount_save();
            ESP_LOGI(TAG, "вид экрана: %s", s_mount ? "на трубе (180°)" : "обычный");
        }
        if (cmd == TFT_CMD_WIFI_SCAN) wifi_scan_run();         // #TFT-41
        if (cmd == TFT_CMD_WIFI_CONNECT) wifi_connect_run();
        if (cmd == TFT_CMD_NETMODE) {             // #TFT-29: как /api/net-mode
            esp_err_t err = wifi_manager_set_boot_mode(s_net_target);
            ESP_LOGW(TAG, "режим сети -> %s (%s), перезагрузка", s_net_target ? "outdoor" : "indoor",
                     esp_err_to_name(err));
            lvgl_port_lock(0);
            toast_show(err == ESP_OK ? "Rebooting..." : "NVS write failed", err == ESP_OK ? COL_ACCENT : COL_RED_TXT);
            lvgl_port_unlock();
            if (err == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(800));   // дать подсказке дойти до экрана
                esp_restart();
            }
        }
        lvgl_port_lock(0);
        if (cmd == TFT_CMD_CAL)      cal_start();      // из веба
        if (cmd == TFT_CMD_CAL_DONE) cal_finish();     // от оверлея: все точки собраны
        if (s_cal_active && (int32_t)(xTaskGetTickCount() - s_cal_deadline) >= 0) cal_abort();
        lvgl_port_unlock();

        bool valid = spectrum_get_snapshot(s_snap);   // копия под SPEC_LOCK
        bool conn  = usb_host_cdc_is_connected();
        bool dead  = usb_host_cdc_spectrometer_dead();
        int state  = valid ? 0 : !conn ? 1 : dead ? 2 : 3;
        bool changed = state != last_state ||
            (valid && (s_snap->total_counts != last_total ||
                       s_snap->total_time_sec != last_time || s_snap->cps != last_cps));
        TickType_t now = xTaskGetTickCount();
        // #TFT-22: статус прибора и батареи — перерисовка при любом изменении.
        battery_status_t bs;
        battery_get_status(&bs);
        int stat = conn | (bs.present << 1) | (bs.charging << 2) | ((bs.present ? bs.percent & 0xff : 0) << 3);
        char ip[16];
        get_ip(ip, sizeof ip);
        bool ip_changed = strcmp(ip, last_ip) != 0;
        if (ip_changed) strcpy(last_ip, ip);
        bool mon_changed = mon_pull();
        // Часы платы выставлены / сменился пояс в вебе — подписи оси X другие.
        int64_t wall0 = monitor_wall_of(0);
        int tz = boot_config_tz_min();
        if (wall0 != last_wall0 || tz != last_tz) mon_changed = true;
        last_wall0 = wall0;
        last_tz = tz;
        int view = s_mount ? VIEW_MNT : s_view;   // #MNT-1: под слоем вкладки не рисуем
        bool view_changed = view != last_view || cmd == TFT_CMD_SCALE || s_force_plot;   // Lin/Log, файл/Live — та же перерисовка
        s_force_plot = false;
        // #TFT-28: строки водопада тянем, только пока открыта его вкладка; на входе
        // окно перечитывается целиком (кольцо платы хранит последние 256 строк).
        if (view == 2 && view_changed) s_wf_synced = false;
        bool wf_changed = view == 2 && wf_pull();
        static bool last_wf_rec;
        if (view == 2 && s_wf_st.recording != last_wf_rec) wf_changed = true;
        last_wf_rec = s_wf_st.recording;
        bool toast_due = s_toast_until && (int32_t)(now - s_toast_until) >= 0;

        // Перерисовка только при изменении: полный кадр спектра — ~180 КБ по SPI.
        // #SYS-1: «прибор|ядро0|ядро1» в %, коротко — «cpu dev|esp» налезал на статус
        // прибора справа (#TFT-25). Загрузка прибора — из STAT свипа.
        char cpu_txt[24];
        int c0 = sys_load_core(0), c1 = sys_load_core(1);
        if (valid) snprintf(cpu_txt, sizeof cpu_txt, "%u|%d|%d", (unsigned)s_snap->cpu_load, c0, c1);
        else       snprintf(cpu_txt, sizeof cpu_txt, "--|%d|%d", c0, c1);
        bool cpu_changed = strcmp(cpu_txt, last_cpu) != 0;
        // #TFT-37: «набор идёт» = прибор подключён и время набора менялось за
        // ACQ_HOLD_MS — так виден и старт из веба/с ПК. Сразу после своей команды —
        // по ней, пока прибор не подтвердил (или ACQ_HINT_MS не прошло).
        if (valid) {
            if (adv_seen && s_snap->total_time_sec != last_time) t_adv = now ? now : 1;
            adv_seen = true;
        }
        bool acq = conn && valid && t_adv && (int32_t)(now - t_adv) < (int32_t)pdMS_TO_TICKS(ACQ_HOLD_MS);
        if (s_acq_hint && (int32_t)(now - s_acq_hint_until) < 0) {
            if ((s_acq_hint > 0) == acq) s_acq_hint = 0;   // прибор подтвердил
            else acq = s_acq_hint > 0;
        } else {
            s_acq_hint = 0;
        }
        int run = view == 0 ? acq : view == 1 ? s_ms.running : view == 2 ? s_wf_st.recording : 0;   // VIEW_MNT — 0
        bool rec_changed = (int)acq != last_acq || run != last_run;
        if (changed || mon_changed || wf_changed || view_changed || stat != last_stat || ip_changed || toast_due ||
            cpu_changed || rec_changed) {
            lvgl_port_lock(0);
#if TFT_PERF
            int64_t perf_lk = esp_timer_get_time();
            ESP_LOGI("tftperf", "loop %lld ms, lock wait %lld ms, dev_time %lu, changed %d",
                     (long long)((perf_t0 - perf_last) / 1000), (long long)((perf_lk - perf_t0) / 1000),
                     (unsigned long)(valid ? s_snap->total_time_sec : 0), (int)changed);
            perf_last = perf_t0;
#endif
            if (changed) render_header(valid);
            // #TFT-32: график спектра — не чаще TFT_PLOT_MIN_MS (было 2 с, как опрос веба),
            // шапка со счётчиком — каждый свип. Полный кадр графика ~450 мс SPI и
            // сотни КБ по шине PSRAM, общей с флешем: вдвое реже — вдвое меньше
            // моргания и помех приёму USB (#TFT-31).
            // #TFT-33: снова раз в секунду (запрос 2026-09-30, после #PERF-1 кадр
            // дешевле); интервал оставлен параметром — вернуть 2 с = 1900 мс.
            bool plot_due = view_changed || state != last_state ||
                            (int32_t)(now - last_plot) >= pdMS_TO_TICKS(TFT_PLOT_MIN_MS);
            // #TFT-34: открыт сохранённый спектр — он не меняется, рисуем только по
            // смене вкладки/шкалы, а не на каждый свип.
            if (view == 0 && (view_changed || (!s_saved_on && changed && plot_due))) {
                bool pv = s_saved_on || valid;
                last_plot = now;
#if TFT_PERF
                int64_t t0 = esp_timer_get_time();
                render_spectrum(pv, MSG[state]);
                ESP_LOGI("tftperf", "render_spectrum under lock %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
#else
                render_spectrum(pv, MSG[state]);
#endif
            }
            if ((view == 1 || view == VIEW_MNT) && (mon_changed || view_changed || state != last_state)) {   // #TFT-32: не на каждый свип
                bool mnt = view == VIEW_MNT;   // #MNT-1: кнопки Start там нет
                if (state)                  snprintf(mon_wait, sizeof mon_wait, "%s", MSG[state]);
                else if (mnt && !s_ms.running) snprintf(mon_wait, sizeof mon_wait, "Double tap to start monitoring");
                else if (!s_ms.start_seq)   snprintf(mon_wait, sizeof mon_wait, "Stopped - press Start");
                else if (s_ms.running)      snprintf(mon_wait, sizeof mon_wait, "Waiting for first %u s interval",
                                                     (unsigned)s_ms.interval_s);
                else                        snprintf(mon_wait, sizeof mon_wait, "Stopped - no intervals");
                if (mnt) mnt_render_monitor(mon_wait);
                else     render_monitor(mon_wait);
            }
            if (view == VIEW_MNT) {   // #MNT-1: крупное CPS — каждый свип, спектр — как вкладка
                if (changed || view_changed) mnt_render_cps(valid);
                if (stat != last_stat || view_changed) mnt_render_status(&bs);
                if (view_changed || (changed && plot_due)) {
                    last_plot = now;
                    mnt_render_spectrum(valid, MSG[state]);
                }
            }
            if (view == 2 && (wf_changed || view_changed)) wf_render();   // #TFT-28
            if (view == 3 && view_changed) conf_render();                   // #TFT-29
            if (stat != last_stat || ip_changed) render_status(conn, &bs, ip);
            if (rec_changed) {   // #TFT-37
                render_rec(acq);
                start_btn_set(run);
                last_acq = acq;
                last_run = run;
            }
            if (cpu_changed) {   // #SYS-1
                lv_label_set_text(s_cpu_val, cpu_txt);
                strcpy(last_cpu, cpu_txt);
            }
            if (toast_due) {
                lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
                s_toast_until = 0;
            }
            lvgl_port_unlock();
        }
        last_state = state;
        last_stat  = stat;
        last_view  = view;
        if (valid) {
            last_total = s_snap->total_counts;
            last_time  = s_snap->total_time_sec;
            last_cps   = s_snap->cps;
        }
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        if (hwm < min_hwm) {
            min_hwm = hwm;
            ESP_LOGI(TAG, "tft_task: стек, минимум свободного %u B из %u", (unsigned)hwm, TFT_TASK_STACK);
        }
    }
}

void tft_display_init(void)
{
#if !TFT_ENABLE
    // #TFT-12: дисплей намеренно выключен — изоляционный тест WiFi.
    ESP_LOGW(TAG, "#TFT-12: TFT_ENABLE=0 — дисплей не поднимается (тест WiFi)");
    return;
#else
    cal_load();             // #TFT-21: до регистрации тача — touch_process() читает s_tcal
    chwin_load();           // #TFT-29: окно каналов экрана из NVS
    mount_load();           // #MNT-1: портрет при старте включает build_ui()
    panel_init();
    if (!s_panel) return;   // panel_init() уже залогировал причину
#if TFT_ENABLE < 2
    // #TFT-12, ступень 1: ни буферов UI, ни фоновой задачи.
    ESP_LOGW(TAG, "#TFT-12: ступень 1 — UI и задача отрисовки НЕ создаются");
    ESP_LOGI(TAG, "internal DMA heap ПОСЛЕ (ступень 1): %u B (макс. блок %u B)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    return;
#else
    s_cmd_q = xQueueCreate(4, sizeof(uint8_t));   // до build_ui(): в неё пишут обработчики касаний
    // #TFT-32: будить и командой, и коммитом свипа. Не вышло — прежний таймер 1 с.
    s_commit_sig = xSemaphoreCreateBinary();
    s_wake_set = (s_cmd_q && s_commit_sig) ? xQueueCreateSet(4 + 1) : NULL;
    if (s_wake_set && xQueueAddToSet(s_cmd_q, s_wake_set) == pdPASS &&
        xQueueAddToSet(s_commit_sig, s_wake_set) == pdPASS) {
        spectrum_add_commit_listener(s_commit_sig);
    } else {
        s_wake_set = NULL;
    }
    if (!s_cmd_q) {
        ESP_LOGE(TAG, "xQueueCreate failed — TFT UI disabled");
        return;
    }
    if (!build_ui()) return;   // build_ui() уже залогировал причину

    if (xTaskCreatePinnedToCore(tft_task, "tft_disp", TFT_TASK_STACK, NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(tft_disp) failed — экран не обновляется");
        return;
    }
    s_ui_ok = true;
    ESP_LOGI(TAG, "TFT init OK (%dx%d, touch=%s, pclk=%d MHz)",
             TFT_H_RES, TFT_V_RES, s_touch ? "yes" : "no", TFT_PCLK_HZ / 1000000);
    // Сколько внутренней памяти осталось системе (WiFi/lwIP/httpd). Если веб
    // не открывается при живом пинге — смотреть сюда в первую очередь.
    ESP_LOGI(TAG, "internal DMA heap ПОСЛЕ: %u B (макс. блок %u B)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
#endif  // TFT_ENABLE < 2
#endif  // !TFT_ENABLE
}
