/*
 * at32f415/floppy.c
 * 
 * Floppy interface control: AT32F415CBT7
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#define O_FALSE 1
#define O_TRUE  0

#define GPO_bus GPO_pushpull(_2MHz,O_FALSE)
#define AFO_bus AFO_pushpull(_2MHz)
#define GPI_bus GPI_floating

/* Input pins */
#define gpio_index  gpiob
#define pin_index   10 /* PB10 */
#define gpio_trk0   gpiob
#define pin_trk0    4 /* PB4 */
#define gpio_wrprot gpiob
#define pin_wrprot  3 /* PB3 */

/* Output pins. */
#define gpio_dir   gpiob
#define pin_dir    8  /* PB8 */
#define gpio_step  gpiob
#define pin_step   6  /* PB6 */
#define gpio_wgate gpiob
#define pin_wgate  7  /* PB7 */
#define gpio_head  gpiob
#define pin_head   5  /* PB5 */

/* RDATA: Pin A15, Timer 2 Channel 1, DMA1 Channel 5. */
#define gpio_rdata  gpioa
#define pin_rdata   15
#define tim_rdata   (tim2)
#define dma_rdata   (dma1->ch5)

/* WDATA: Pin A2, Timer 2 Channel 3, DMA1 Channel 2. */
#define gpio_wdata  gpioa
#define pin_wdata   2
#define tim_wdata   (tim2)
#define dma_wdata   (dma1->ch2)

typedef uint16_t timcnt_t;

#define irq_index 40
void IRQ_40(void) __attribute__((alias("IRQ_INDEX_changed"))); /* EXTI15_10 */

const struct pin_mapping msel_pins[] = {
    { 10, _A,  3 },
    { 12, _B,  9 },
    { 14, _A,  4 },
    { 16, _A,  1 },
    {  0,  0,  0 }
};

const struct pin_mapping user_pins[] = {
    {  2, _A,  6 },
    {  4, _A,  5 },
    {  6, _A,  7 },
    {  0,  0,  0 }
};

/* We sometimes cast u_buf to uint32_t[], hence the alignment constraint. */
#define U_BUF_SZ 16384
static uint8_t u_buf[U_BUF_SZ] aligned(4);

static void floppy_mcu_init(void)
{
    const struct pin_mapping *mpin;
    const struct pin_mapping *upin;

    /* Map PA15 -> TIM2 Ch1. */
    afio->mapr = (AFIO_MAPR_SWJ_ON_JTAG_OFF
                  | AFIO_MAPR_TIM2_REMAP_PARTIAL_1);

    /* Enable clock for Timer 2. */
    rcc->apb1enr |= RCC_APB1ENR_TIM2EN;

    configure_pin(rdata, GPI_bus);

    /* Configure user-modifiable pins. */
    for (upin = user_pins; upin->pin_id != 0; upin++) {
        gpio_configure_pin(gpio_from_id(upin->gpio_bank), upin->gpio_pin,
                           GPO_bus);
    }

    /* Configure SELECT/MOTOR lines. */
    for (mpin = msel_pins; mpin->pin_id != 0; mpin++) {
        gpio_configure_pin(gpio_from_id(mpin->gpio_bank), mpin->gpio_pin,
                           GPO_bus);
    }

    /* Set up EXTI mapping for INDEX: PB[11:8] -> EXT[11:8] */
    afio->exticr3 = 0x1111;
}

static void rdata_prep(void)
{
    /* RDATA Timer setup: 
     * The counter runs from 0x0000-0xFFFF inclusive at SAMPLE rate.
     *  
     * Ch.1 (RDATA) is in Input Capture mode, sampling on every clock and with
     * no input prescaling or filtering. Samples are captured on the falling 
     * edge of the input (CCxP=1). DMA is used to copy the sample into a ring
     * buffer for batch processing in the DMA-completion ISR. */
    tim_rdata->psc = TIM_PSC-1;
    tim_rdata->arr = 0xffff;
    tim_rdata->ccmr1 = TIM_CCMR1_CC1S(TIM_CCS_INPUT_TI1);
    tim_rdata->dier = TIM_DIER_CC1DE;
    tim_rdata->cr2 = 0;
    tim_rdata->egr = TIM_EGR_UG; /* update CNT, PSC, ARR */
    tim_rdata->sr = 0; /* dummy write */

    /* RDATA DMA setup: From the RDATA Timer's CCRx into a circular buffer. */
    dma_rdata.par = (uint32_t)(unsigned long)&tim_rdata->ccr1;
    dma_rdata.cr = (DMA_CR_PL_HIGH |
                    DMA_CR_MSIZE_16BIT |
                    DMA_CR_PSIZE_16BIT |
                    DMA_CR_MINC |
                    DMA_CR_CIRC |
                    DMA_CR_DIR_P2M |
                    DMA_CR_EN);

    tim_rdata->ccer = TIM_CCER_CC1E | TIM_CCER_CC1P;
}

static void wdata_prep(void)
{
    /* WDATA Timer setup:
     * The counter is incremented at SAMPLE rate. 
     *  
     * Ch.3 (WDATA) is in PWM mode 1. It outputs O_TRUE for 400ns and then 
     * O_FALSE until the counter reloads. By changing the ARR via DMA we alter
     * the time between (fixed-width) O_TRUE pulses, mimicking floppy drive 
     * timings. */
    tim_wdata->psc = TIM_PSC-1;
    tim_wdata->ccmr2 = (TIM_CCMR2_CC3S(TIM_CCS_OUTPUT) |
                        TIM_CCMR2_OC3M(TIM_OCM_PWM1));
    tim_wdata->ccer = TIM_CCER_CC3E | ((O_TRUE==0) ? TIM_CCER_CC3P : 0);
    tim_wdata->ccr3 = sample_ns(400);
    tim_wdata->dier = TIM_DIER_UDE;
    tim_wdata->cr2 = 0;
}

static void dma_wdata_start(void)
{
    dma_wdata.cr = (DMA_CR_PL_HIGH |
                    DMA_CR_MSIZE_16BIT |
                    DMA_CR_PSIZE_16BIT |
                    DMA_CR_MINC |
                    DMA_CR_CIRC |
                    DMA_CR_DIR_M2P |
                    DMA_CR_EN);
}

static void drive_deselect(void)
{
    int pin = -1;
    uint8_t rc;

    if (unit_nr == -1)
        return;

    switch (bus_type) {
    case BUS_IBMPC:
        switch (unit_nr) {
        case 0: pin = 14; break;
        case 1: pin = 12; break;
        }
        break;
    case BUS_SHUGART:
        switch (unit_nr) {
        case 0: pin = 10; break;
        case 1: pin = 12; break;
        case 2: pin = 14; break;
        }
        break;
    }

    rc = write_mapped_pin(msel_pins, pin, O_FALSE);
    ASSERT(rc == ACK_OKAY);

    unit_nr = -1;
}

static uint8_t drive_select(uint8_t nr)
{
    int pin = -1;
    uint8_t rc;

    if (nr == unit_nr)
        return ACK_OKAY;

    drive_deselect();

    switch (bus_type) {
    case BUS_IBMPC:
        switch (nr) {
        case 0: pin = 14; break;
        case 1: pin = 12; break;
        default: return ACK_BAD_UNIT;
        }
        break;
    case BUS_SHUGART:
        switch (nr) {
        case 0: pin = 10; break;
        case 1: pin = 12; break;
        case 2: pin = 14; break;
        default: return ACK_BAD_UNIT;
        }
        break;
    default:
        return ACK_NO_BUS;
    }

    rc = write_mapped_pin(msel_pins, pin, O_TRUE);
    if (rc != ACK_OKAY)
        return ACK_BAD_UNIT;

    unit_nr = nr;
    delay_us(delay_params.select_delay);

    return ACK_OKAY;
}

static uint8_t drive_motor(uint8_t nr, bool_t on)
{
    int pin = -1;
    uint8_t rc;

    switch (bus_type) {
    case BUS_IBMPC:
        if (nr >= 2) 
            return ACK_BAD_UNIT;
        if (unit[nr].motor == on)
            return ACK_OKAY;
        switch (nr) {
        case 0: pin = 10; break;
        case 1: pin = 16; break;
        }
        break;
    case BUS_SHUGART:
        if (nr >= 3)
            return ACK_BAD_UNIT;
        /* All shugart units share one motor line. Alias them all to unit 0. */
        nr = 0;
        if (unit[nr].motor == on)
            return ACK_OKAY;
        pin = 16;
        break;
    default:
        return ACK_NO_BUS;
    }

    rc = write_mapped_pin(msel_pins, pin, on ? O_TRUE : O_FALSE);
    if (rc != ACK_OKAY)
        return ACK_BAD_UNIT;

    unit[nr].motor = on;
    if (on)
        delay_ms(delay_params.motor_delay);

    return ACK_OKAY;

}

static uint8_t mcu_get_floppy_pin(unsigned int pin, uint8_t *p_level)
{
    if (pin == 34) {
        *p_level = gpio_read_pin(gpiob, 15);
        return ACK_OKAY;
    }
    return ACK_BAD_PIN;
}

static uint8_t set_user_pin(unsigned int pin, unsigned int level)
{
    const struct pin_mapping *upin;

    for (upin = user_pins; upin->pin_id != 0; upin++) {
        if (upin->pin_id == pin)
            goto found;
    }
    return ACK_BAD_PIN;

found:
    gpio_write_pin(gpio_from_id(upin->gpio_bank), upin->gpio_pin, level);
    return ACK_OKAY;
}

static void reset_user_pins(void)
{
    const struct pin_mapping *upin;

    for (upin = user_pins; upin->pin_id != 0; upin++)
        gpio_write_pin(gpio_from_id(upin->gpio_bank), upin->gpio_pin, O_FALSE);
}

static void flippy_trk0_sensor(bool_t level)
{
    gpio_write_pin(gpiob, 14, level);
    delay_us(10);
}

#define flippy_trk0_sensor_disable() flippy_trk0_sensor(HIGH)
#define flippy_trk0_sensor_enable() flippy_trk0_sensor(LOW)

static bool_t flippy_detect(void)
{
    bool_t is_flippy;
    flippy_trk0_sensor_disable();
    is_flippy = (get_trk0() == HIGH);
    flippy_trk0_sensor_enable();
    return is_flippy;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
