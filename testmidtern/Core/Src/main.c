/* USER CODE BEGIN Header */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef enum { ST_SET, ST_NORMAL, ST_WAITING, ST_SUCCESS, ST_LOCK } State_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* ---- 腳位 ---- */
#define LED_G_PORT      GPIOA
#define LED_G_PIN       GPIO_PIN_12
#define LED_Y_PORT      GPIOC
#define LED_Y_PIN       GPIO_PIN_6
#define LED_R_PORT      GPIOC
#define LED_R_PIN       GPIO_PIN_8
#define BTN_PORT        GPIOA
#define BTN_PIN         GPIO_PIN_11      /* 按下 = 低電位 (上拉) */

/* ---- 行為切換 ---- */
#define NORMAL_GREEN_BLINK  1   /* 1: 綠燈 1Hz 閃, 0: 恆亮 */
#define SET_GREEN_ON        0   /* 1: SET 狀態綠燈恆亮, 0: 全滅 */
#define NORMAL_WRONG_LOCKS  1   /* 1: NORMAL 直接輸入錯誤進 LOCK, 0: 維持 NORMAL */

/* ---- 時間 (ms) ---- */
#define WAIT_MS         5000u
#define LOCK_MS         30000u
#define SUCCESS_MS      3000u
#define G_BLINK_MS      500u    /* 1 Hz */
#define Y_BLINK_MS      100u    /* 5 Hz */
#define R_BLINK_MS      250u    /* 紅燈閃爍 2 Hz */
#define BREATH_MS       1500u   /* 一次呼吸週期, 3 秒 = 2 次 */
#define BTN_DEBOUNCE_MS 20u
#define EXTI_DEBOUNCE_MS 200u

#define PWM_ARR         999u
#define PW_MAX_LEN      16u
#define RX_BUF_SIZE     64u     /* 需為 256 的因數 */

/* ---- 訊息 ---- */
#define MSG_SET_PROMPT  "PLEASE SET THE PASSWORD\r\n"
#define MSG_SET_OK      "PASSWORD SET SUCCESSFULLY\r\n"
#define MSG_PROMPT      "PLEASE ENTER THE PASSWORD\r\n"
#define MSG_GRANTED     "ACCESS GRANTED - WELCOME AGENT!\r\n"
#define MSG_WAIT        "YOU HAVE 5 SECOND TO ENTER THE PASSWORD\r\n"
#define MSG_LOCK        "LOCK IN 30SEC - WRONG PASSWORD\r\n"
#define MSG_EMERGENCY   "EMERGENCY TRIGGERED! LOCK IN 30SEC\r\n"
#define MSG_TOO_LONG    "PASSWORD TOO LONG (MAX 16)\r\n"
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef hlpuart1;

TIM_HandleTypeDef htim3;

/* USER CODE BEGIN PV */
static State_t  g_state = ST_SET;
static uint32_t g_state_t0 = 0;
static uint32_t g_blink_t0 = 0;

static uint8_t  g_pw[PW_MAX_LEN];
static uint8_t  g_pw_len = 0;            /* 0 = 尚未設定密碼 */
static uint8_t  g_in[PW_MAX_LEN];
static uint8_t  g_in_len = 0;
static uint8_t  g_in_over = 0;           /* 輸入超過長度 */

static volatile uint8_t  g_rx_buf[RX_BUF_SIZE];
static volatile uint8_t  g_rx_head = 0;
static volatile uint8_t  g_rx_tail = 0;
static uint8_t  g_rx_byte;

static volatile uint8_t  g_emergency = 0;
static volatile uint32_t g_exti_last = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_LPUART1_UART_Init(void);
static void MX_TIM3_Init(void);
/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ================= UART 輸出 (不使用 string.h) ================= */
static void uart_puts(const char *s)
{
    uint16_t n = 0;
    while (s[n] != '\0') { n++; }
    HAL_UART_Transmit(&hlpuart1, (uint8_t *)s, n, 100);
}

static void uart_putc(uint8_t c)
{
    HAL_UART_Transmit(&hlpuart1, &c, 1, 10);
}

/* ================= UART 接收環形緩衝 ================= */
static uint8_t rx_pop(uint8_t *c)
{
    if (g_rx_head == g_rx_tail) { return 0; }
    *c = g_rx_buf[g_rx_tail];
    g_rx_tail = (uint8_t)((g_rx_tail + 1u) % RX_BUF_SIZE);
    return 1;
}

static void rx_flush(void)
{
    g_rx_tail = g_rx_head;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == LPUART1)
    {
        uint8_t next = (uint8_t)((g_rx_head + 1u) % RX_BUF_SIZE);
        if (next != g_rx_tail)
        {
            g_rx_buf[g_rx_head] = g_rx_byte;
            g_rx_head = next;
        }
        HAL_UART_Receive_IT(&hlpuart1, &g_rx_byte, 1);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == LPUART1)
    {
        HAL_UART_Receive_IT(&hlpuart1, &g_rx_byte, 1);   /* overrun 等錯誤後重新接收 */
    }
}

/* ================= PB12 緊急按鈕 (EXTI12) ================= */
static void emergency_isr(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_12)
    {
        uint32_t now = HAL_GetTick();
        if ((now - g_exti_last) >= EXTI_DEBOUNCE_MS)     /* 軟體去彈跳 */
        {
            g_exti_last = now;
            g_emergency = 1;                             /* 交給主迴圈處理, ISR 內不傳輸 */
        }
    }
}

/* 舊版 HAL 呼叫這個 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    emergency_isr(GPIO_Pin);
}

/* 新版 HAL 呼叫這個 */
void HAL_GPIO_EXTI_Falling_Callback(uint16_t GPIO_Pin)
{
    emergency_isr(GPIO_Pin);
}

/* ================= PA11 按鈕 polling + 去彈跳 ================= */
static uint8_t btn_press_event(void)
{
    static GPIO_PinState last_raw = GPIO_PIN_SET;
    static GPIO_PinState stable   = GPIO_PIN_SET;
    static uint32_t t_change = 0;

    GPIO_PinState raw = HAL_GPIO_ReadPin(BTN_PORT, BTN_PIN);
    uint32_t now = HAL_GetTick();

    if (raw != last_raw)
    {
        last_raw = raw;
        t_change = now;
    }
    else if ((raw != stable) && ((now - t_change) >= BTN_DEBOUNCE_MS))
    {
        stable = raw;
        if (stable == GPIO_PIN_RESET) { return 1; }      /* 穩定的按下事件 */
    }
    return 0;
}

/* ================= LED ================= */
static void leds_off(void)
{
    HAL_GPIO_WritePin(LED_G_PORT, LED_G_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LED_Y_PORT, LED_Y_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LED_R_PORT, LED_R_PIN, GPIO_PIN_RESET);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, 0);
}

static void leds_update(void)
{
    uint32_t now = HAL_GetTick();

    switch (g_state)
    {
    case ST_NORMAL:
#if NORMAL_GREEN_BLINK
        if ((now - g_blink_t0) >= G_BLINK_MS)
        {
            g_blink_t0 = now;
            HAL_GPIO_TogglePin(LED_G_PORT, LED_G_PIN);
        }
#endif
        break;

    case ST_WAITING:
        if ((now - g_blink_t0) >= Y_BLINK_MS)
        {
            g_blink_t0 = now;
            HAL_GPIO_TogglePin(LED_Y_PORT, LED_Y_PIN);
        }
        break;

    case ST_LOCK:
        if ((now - g_blink_t0) >= R_BLINK_MS)
        {
            g_blink_t0 = now;
            HAL_GPIO_TogglePin(LED_R_PORT, LED_R_PIN);
        }
        break;

    case ST_SUCCESS:
    {
        /* 三角波 + 平方, 讓亮度變化看起來比較線性 */
        uint32_t half = BREATH_MS / 2u;
        uint32_t p    = (now - g_state_t0) % BREATH_MS;
        uint32_t tri  = (p < half) ? p : (BREATH_MS - p);
        uint32_t ccr  = (tri * tri * PWM_ARR) / (half * half);
        __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, ccr);
        break;
    }

    default:
        break;
    }
}

/* ================= 狀態機 ================= */
static void enter_state(State_t s)
{
    g_state    = s;
    g_state_t0 = HAL_GetTick();
    g_blink_t0 = g_state_t0;
    g_in_len   = 0;
    g_in_over  = 0;
    rx_flush();
    leds_off();

    switch (s)
    {
    case ST_SET:
#if SET_GREEN_ON
        HAL_GPIO_WritePin(LED_G_PORT, LED_G_PIN, GPIO_PIN_SET);
#endif
        uart_puts(MSG_SET_PROMPT);           /* PLEASE SET THE PASSWORD */
        break;

    case ST_NORMAL:
        HAL_GPIO_WritePin(LED_G_PORT, LED_G_PIN, GPIO_PIN_SET);  /* 閃爍模式由亮開始 */
        uart_puts(MSG_PROMPT);               /* PLEASE ENTER THE PASSWORD */
        break;

    case ST_WAITING:
        HAL_GPIO_WritePin(LED_Y_PORT, LED_Y_PIN, GPIO_PIN_SET);
        uart_puts(MSG_WAIT);
        break;

    case ST_SUCCESS:
        /* 藍燈由 leds_update() 做呼吸 */
        uart_puts(MSG_GRANTED);
        break;

    case ST_LOCK:
        HAL_GPIO_WritePin(LED_R_PORT, LED_R_PIN, GPIO_PIN_SET);
        /* 訊息由 enter_lock() 依原因傳送 */
        break;
    }
}

/* 進入 LOCK 並傳送對應訊息 (錯誤/逾時 或 緊急按鈕) */
static void enter_lock(const char *msg)
{
    enter_state(ST_LOCK);
    uart_puts(msg);
}

static void back_to_idle(void)
{
    enter_state((g_pw_len == 0u) ? ST_SET : ST_NORMAL);
}

static uint8_t pw_match(void)
{
    uint8_t diff = 0;
    if (g_in_over || (g_in_len != g_pw_len)) { return 0; }
    for (uint8_t i = 0; i < g_pw_len; i++)
    {
        diff |= (uint8_t)(g_in[i] ^ g_pw[i]);
    }
    return (diff == 0u) ? 1 : 0;
}

static void input_submit(void)
{
    uart_puts("\r\n");

    if (g_state == ST_SET)
    {
        if (g_in_over)
        {
            uart_puts(MSG_TOO_LONG);
            g_in_len = 0;
            g_in_over = 0;
            uart_puts(MSG_SET_PROMPT);
            return;
        }
        for (uint8_t i = 0; i < g_in_len; i++) { g_pw[i] = g_in[i]; }
        g_pw_len = g_in_len;
        uart_puts(MSG_SET_OK);
        enter_state(ST_NORMAL);              /* 綠燈閃爍 + PLEASE ENTER THE PASSWORD */
        return;
    }

    /* ST_NORMAL / ST_WAITING */
    if (pw_match())
    {
        enter_state(ST_SUCCESS);             /* ACCESS GRANTED - WELCOME AGENT! */
    }
    else if ((g_state == ST_WAITING) || NORMAL_WRONG_LOCKS)
    {
        enter_lock(MSG_LOCK);
    }
    else
    {
        g_in_len = 0;
        g_in_over = 0;
    }
}

static void input_process(void)
{
    uint8_t c;
    while (rx_pop(&c))
    {
        if ((g_state == ST_LOCK) || (g_state == ST_SUCCESS))
        {
            rx_flush();                      /* 忽略所有輸入 */
            return;
        }

        if ((c == '\r') || (c == '\n'))
        {
            if ((g_in_len == 0u) && !g_in_over) { continue; }   /* 空輸入或 CRLF 的第二個字元 */
            input_submit();
        }
        else if ((c == 0x08) || (c == 0x7F))                    /* Backspace */
        {
            if (g_in_len > 0u)
            {
                g_in_len--;
                uart_puts("\b \b");
            }
        }
        else if ((c >= 0x20) && (c <= 0x7E))
        {
            if (g_in_len < PW_MAX_LEN) { g_in[g_in_len++] = c; }
            else                       { g_in_over = 1; }
            uart_putc('*');
        }
    }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_LPUART1_UART_Init();
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_4);
  HAL_UART_Receive_IT(&hlpuart1, &g_rx_byte, 1);
  enter_state(ST_SET);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    uint8_t pa11 = btn_press_event();        /* 每圈都呼叫, 維持去彈跳狀態 */

    /* PB12: 任何狀態都生效 (LOCK 中不重複觸發) */
    if (g_emergency)
    {
        g_emergency = 0;
        if (g_state != ST_LOCK) { enter_lock(MSG_EMERGENCY); }
    }

    uint32_t now = HAL_GetTick();            /* 放在 enter_state 之後, 避免時間倒退 */

    switch (g_state)
    {
    case ST_SET:
        input_process();
        break;

    case ST_NORMAL:
        if (pa11) { enter_state(ST_WAITING); }
        else      { input_process(); }
        break;

    case ST_WAITING:
        input_process();
        if ((g_state == ST_WAITING) && ((now - g_state_t0) >= WAIT_MS))
        {
            enter_lock(MSG_LOCK);            /* 5 秒逾時 */
        }
        break;

    case ST_SUCCESS:
        rx_flush();
        if ((now - g_state_t0) >= SUCCESS_MS) { back_to_idle(); }
        break;

    case ST_LOCK:
        rx_flush();
        if ((now - g_state_t0) >= LOCK_MS) { back_to_idle(); }
        break;
    }

    leds_update();
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV4;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief LPUART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_LPUART1_UART_Init(void)
{

  /* USER CODE BEGIN LPUART1_Init 0 */
  /* USER CODE END LPUART1_Init 0 */

  /* USER CODE BEGIN LPUART1_Init 1 */
  /* USER CODE END LPUART1_Init 1 */
  hlpuart1.Instance = LPUART1;
  hlpuart1.Init.BaudRate = 115200;
  hlpuart1.Init.WordLength = UART_WORDLENGTH_8B;
  hlpuart1.Init.StopBits = UART_STOPBITS_1;
  hlpuart1.Init.Parity = UART_PARITY_NONE;
  hlpuart1.Init.Mode = UART_MODE_TX_RX;
  hlpuart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  hlpuart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  hlpuart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  hlpuart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&hlpuart1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&hlpuart1, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&hlpuart1, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&hlpuart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN LPUART1_Init 2 */
  /* USER CODE END LPUART1_Init 2 */

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */
  /* USER CODE END TIM3_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM3_Init 1 */
  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 83;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 999;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 250;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_4) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */
  /* USER CODE END TIM3_Init 2 */
  HAL_TIM_MspPostInit(&htim3);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */
  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_6|GPIO_PIN_8, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_RESET);

  /*Configure GPIO pin : PB12 */
  GPIO_InitStruct.Pin = GPIO_PIN_12;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;      /* 原為 RISING, 上拉按鈕按下是下降沿 */
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : PC6 PC8 */
  GPIO_InitStruct.Pin = GPIO_PIN_6|GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : PA11 */
  GPIO_InitStruct.Pin = GPIO_PIN_11;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : PA12 */
  GPIO_InitStruct.Pin = GPIO_PIN_12;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 0, 0);       /* 原為 1, 依需求改為最高 */
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
