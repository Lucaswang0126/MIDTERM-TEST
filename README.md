# MIDTERM-TEST：STM32G474RE 密碼鎖

以 STM32 NUCLEO-G474RE 實作的密碼鎖專案，使用 C 語言與 HAL 函式庫，不使用 `string.h`、`sprintf` 等字串函式，密碼與輸入全部以 `uint8_t` 陣列處理。

專案內容位於 [`testmidtern/`](./testmidtern) 資料夾。

## 功能

- 第一次開機先設定密碼，之後進入驗證模式。
- 以 LPUART1 與電腦終端機互動，輸入時顯示 `*`，支援 Backspace。
- PA11 按鈕（Polling + 軟體去彈跳）進入 5 秒限時驗證。
- PB12 緊急按鈕（EXTI 中斷）在任何狀態都會立刻鎖定 30 秒。
- 四色 LED 顯示狀態，藍燈使用 TIM3 PWM 做呼吸燈。

## 硬體與腳位

| 腳位 | 功能 | 說明 |
|------|------|------|
| PA11 | 按鈕輸入 | 內部上拉，按下為低電位，Polling |
| PB12 | 緊急按鈕 | EXTI12，下降沿觸發，內部上拉 |
| PA12 | 綠燈 | GPIO 輸出 |
| PC6 | 黃燈 | GPIO 輸出 |
| PC8 | 紅燈 | GPIO 輸出 |
| PC9 | 藍燈 | TIM3_CH4 PWM |
| PA2 | LPUART1_TX | 115200 bps |
| PA3 | LPUART1_RX | 115200 bps |

## 週邊設定

| 項目 | 設定 |
|------|------|
| 時脈 | HSI 16 MHz → PLL（/4、×85、/2）→ SYSCLK 170 MHz |
| 除錯介面 | Serial Wire |
| 時基 | SysTick |
| LPUART1 | 非同步，115200 bps，8N1，無流控 |
| TIM3 | CH4 PWM，PSC = 83，ARR = 999，PWM Mode 1，極性 High（約 2 kHz） |
| NVIC | TIM3、LPUART1、EXTI[15:10] 啟用，EXTI 優先權最高 |

## 狀態機

| 狀態 | LED | 說明 |
|------|-----|------|
| SET | 全滅 | 第一次設定密碼 |
| NORMAL | 綠燈 1 Hz 閃爍 | 待機，可直接輸入密碼 |
| WAITING | 黃燈 5 Hz 閃爍 | 按下 PA11 後，限時 5 秒輸入 |
| SUCCESS | 藍燈呼吸 3 秒 | 密碼正確 |
| LOCK | 紅燈閃爍 30 秒 | 忽略所有輸入 |

### 狀態轉換

- SET：設定完成 → NORMAL
- NORMAL：按 PA11 → WAITING；直接輸入正確 → SUCCESS；直接輸入錯誤 → LOCK
- WAITING：密碼正確 → SUCCESS；密碼錯誤或 5 秒逾時 → LOCK
- SUCCESS：3 秒後 → NORMAL
- LOCK：30 秒後 → NORMAL（尚未設定密碼則回到 SET）
- 任何狀態按 PB12 → LOCK

## 終端機訊息

| 情境 | 訊息 |
|------|------|
| 開機（尚未設定） | `PLEASE SET THE PASSWORD` |
| 設定完成 | `PASSWORD SET SUCCESSFULLY` |
| 回到待機 | `PLEASE ENTER THE PASSWORD` |
| 按下 PA11 | `YOU HAVE 5 SECOND TO ENTER THE PASSWORD` |
| 密碼正確 | `ACCESS GRANTED - WELCOME AGENT!` |
| 密碼錯誤或逾時 | `LOCK IN 30SEC - WRONG PASSWORD` |
| 按下 PB12 | `EMERGENCY TRIGGERED! LOCK IN 30SEC` |

## 檔案結構

```text
MIDTERM-TEST/
├── README.md
└── testmidtern/
    ├── testmidtern.ioc            # CubeMX 設定檔（腳位、時脈、NVIC）
    ├── Core/
    │   ├── Inc/
    │   │   ├── main.h
    │   │   ├── stm32g4xx_hal_conf.h
    │   │   └── stm32g4xx_it.h
    │   ├── Src/
    │   │   ├── main.c                # 密碼鎖主程式（狀態機、UART、按鈕、LED）
    │   │   ├── stm32g4xx_hal_msp.c   # 週邊底層初始化（LPUART1、TIM3 腳位與中斷）
    │   │   ├── stm32g4xx_it.c        # 中斷服務函式（EXTI、LPUART1、SysTick）
    │   │   ├── system_stm32g4xx.c
    │   │   └── syscalls.c / sysmem.c
    │   └── Startup/
    │       └── startup_stm32g474retx.s
    ├── Drivers/
    │   ├── CMSIS/                    # ARM / ST 核心標頭檔
    │   └── STM32G4xx_HAL_Driver/     # HAL 函式庫
    └── STM32G474RETX_FLASH.ld        # 連結腳本
```

主要程式都在 `Core/Src/main.c`，使用者程式碼寫在各 `USER CODE BEGIN/END` 區塊內，重新產生程式碼時不會被覆蓋。

## 使用方式

1. 以 STM32CubeIDE 開啟專案並編譯、燒錄到 NUCLEO-G474RE。
2. 用終端機軟體（Tera Term、PuTTY 等）連接板子的虛擬 COM 埠，設定 115200、8N1、無流控，建議關閉 local echo。
3. 上電後輸入密碼（最長 16 字元）並按 Enter 完成設定。
4. 在綠燈閃爍時直接輸入密碼，或先按 PA11 再於 5 秒內輸入。
5. 隨時可按 PB12 觸發緊急鎖定。

## 程式內可調整的選項

在 `main.c` 的 `USER CODE BEGIN PD` 區塊：

| 巨集 | 預設 | 說明 |
|------|------|------|
| `NORMAL_GREEN_BLINK` | 1 | 1：待機綠燈 1 Hz 閃爍；0：恆亮 |
| `SET_GREEN_ON` | 0 | 1：設定狀態綠燈恆亮；0：全滅 |
| `NORMAL_WRONG_LOCKS` | 1 | 1：待機時直接輸入錯誤會鎖定；0：維持待機 |
| `WAIT_MS` / `LOCK_MS` / `SUCCESS_MS` | 5000 / 30000 / 3000 | 各狀態時間（ms） |
| `PW_MAX_LEN` | 16 | 密碼最大長度 |

## 注意事項

- 密碼只存在 RAM，重新上電或重置後需要再設定一次。
- LED 假設為高電位點亮；若為低電位點亮，需對調 `SET`/`RESET`，並把 TIM3 PWM 極性改為 Low。
- PB12 的邊緣（Falling）與 EXTI 優先權在 CubeMX 產生的區塊內，修改 `.ioc` 後重新產生程式碼時，請在 CubeMX 內同步設定。
- 緊急按鈕在 ISR 內只設旗標，傳輸訊息與狀態切換都在主迴圈處理。
