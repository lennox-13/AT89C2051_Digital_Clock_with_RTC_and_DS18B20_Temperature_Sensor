#include <reg2051.h>
#include <intrins.h>

typedef unsigned char uint8_t;
typedef unsigned int  uint16_t;

/* --- Konstanty --- */
#define CLOCK_TIMER_HIGH  0x3C
#define CLOCK_TIMER_LOW   0xD5
#define BUTTON_PRESS      2
#define BUTTON_PRESS_LONG 40
#define LED_BLANK         10
#define LED_MINUS         11
#define LED_C             12
#define DP_BIT            0x01

/* =======================================================================
   1-WIRE / DS18B20
======================================================================= */
#define DLY5US  _nop_();_nop_();_nop_();_nop_();_nop_()
#define DLY10US DLY5US;DLY5US
#define DLY60US DLY10US;DLY10US;DLY10US;DLY10US;DLY10US;DLY10US
#define DLY70US DLY60US;DLY10US
#define DLY_LONG(us) { uint16_t _d; for(_d=0;_d<(us>>1);_d++){_nop_();} }

/* --- Piny --- */
sbit SR_DATA     = P3^0;
sbit SR_CLK      = P3^1;
sbit DS1302_SCLK = P3^2;
sbit DS1302_IO   = P3^3;
sbit OW          = P3^5;
sbit DS1302_CE   = P3^7;

/* --- Tabulka segmentov --- */
const uint8_t ledtable[] = {
    0xFC,0x24,0xBA,0xAE,0x66,0xCE,0xDE,0xA4,0xFE,0xEE,
    0x00,  /* 10=blank */
    0x02,  /* 11='-'   */
    0xD8   /* 12='C'   */
};

uint8_t dbuf[4];

/* --- Hodiny --- */
volatile uint8_t clock_hour   = 0;
volatile uint8_t clock_minute = 0;
volatile uint8_t clock_second = 0;
volatile bit     show_colon   = 0;

/* --- Editacia --- */
volatile uint8_t edit_blink_cnt = 0;
volatile bit     edit_blink     = 1;
volatile uint8_t repeat_tick    = 0;
volatile bit     clock_increment = 0;
volatile uint8_t edit_hour;
volatile uint8_t edit_min;

/* --- Tlacidla --- */
volatile uint8_t debounce[2] = {0,0};
volatile bit B1_PRESSED      = 0;
volatile bit B1_RELEASED     = 0;
volatile bit B1_PRESSED_LONG = 0;
volatile bit B2_PRESSED      = 0;
volatile bit B2_RELEASED     = 0;
volatile bit B2_PRESSED_LONG = 0;
volatile bit B2_RELEASED_LONG= 0;

/* --- Stavy --- */
typedef enum { NORMAL, TEMPERATURE, MIN_SEC, EDIT_HOUR, EDIT_MIN } clock_state_t;
clock_state_t clock_state = NORMAL;

/* =======================================================================
   SHIFT REGISTER
======================================================================= */
void sr_output(uint8_t p) {
    uint8_t i;
    for(i=0;i<8;i++) { SR_DATA=(p&0x01)?1:0; SR_CLK=1; SR_CLK=0; p>>=1; }
}
void sr_all_off(void)           { sr_output(0x00); }
void sr_select_digit(uint8_t d) { sr_output(1<<d); }

/* =======================================================================
   DS1302
======================================================================= */
void ds1302_delay(void) { uint8_t i; for(i=0;i<5;i++){;} }

void ds1302_write_byte(uint8_t v) {
    uint8_t i;
    for(i=0;i<8;i++) {
        DS1302_SCLK=0; DS1302_IO=(v&0x01); ds1302_delay();
        DS1302_SCLK=1; ds1302_delay();
        DS1302_SCLK=0; ds1302_delay();
        v>>=1;
    }
}

uint8_t ds1302_read_byte(void) {
    uint8_t i,r=0;
    DS1302_IO=1;
    for(i=0;i<8;i++) {
        DS1302_SCLK=1; ds1302_delay();
        if(DS1302_IO) r|=(1<<i);
        DS1302_SCLK=0; ds1302_delay();
    }
    return r;
}

uint8_t ds1302_read_reg(uint8_t addr) {
    uint8_t v;
    DS1302_SCLK=0; DS1302_CE=0; ds1302_delay();
    DS1302_CE=1; ds1302_delay();
    ds1302_write_byte(addr); v=ds1302_read_byte();
    DS1302_CE=0; ds1302_delay();
    return v;
}

void ds1302_write_reg(uint8_t addr, uint8_t val) {
    DS1302_SCLK=0; DS1302_CE=0; ds1302_delay();
    DS1302_CE=1; ds1302_delay();
    ds1302_write_byte(addr); ds1302_write_byte(val);
    DS1302_CE=0; ds1302_delay();
}

void ds1302_read_time(void) {
    uint8_t s,m,h;
    s=ds1302_read_reg(0x81)&0x7F;
    m=ds1302_read_reg(0x83)&0x7F;
    h=ds1302_read_reg(0x85);
    s=(s&0x0F)+((s>>4)*10);
    m=(m&0x0F)+((m>>4)*10);
    if(s>59||m>59) return;
    h=(h&0x0F)+((h>>4)*10);
    if(h>23) return;
    clock_second=s; clock_minute=m; clock_hour=h;
}

void ds1302_write_time(uint8_t h, uint8_t m) {
    uint8_t bh=((h/10)<<4)|(h%10);
    uint8_t bm=((m/10)<<4)|(m%10);
    ds1302_write_reg(0x8E,0x00);
    ds1302_write_reg(0x80,0x00);
    ds1302_write_reg(0x82,bm);
    ds1302_write_reg(0x84,bh);
    ds1302_write_reg(0x8E,0x80);
}

void ds1302_init_default(void) {
    uint8_t s=ds1302_read_reg(0x81);
    if(s&0x80) {
        ds1302_write_reg(0x8E,0x00);
        ds1302_write_reg(0x80,s&0x7F);
        ds1302_write_reg(0x8E,0x80);
    }
}

/* =======================================================================
   DISPLAY
======================================================================= */
void delay1ms(void) { uint16_t i; for(i=0;i<150;i++){;} }

void display_update(void) {
    uint8_t d;
    for(d=0;d<4;d++) {
        P1=0x00; sr_all_off(); sr_select_digit(d);
        P1=dbuf[d]; delay1ms();
    }
    P1=0x00;
}

/* =======================================================================
   Zobrazenie teplot na displeji
   Kladna: 0.0C az 75.0C   
	 Zaporna: -0.1C az -9.9C 
	 Zaporna: <=-10C         
   Chyba: - - - -                 
======================================================================= */
void set_temp_dbuf(uint16_t tr) {
    uint8_t bcd,dec,tens,units;
    if(tr==0xFFFF) { dbuf[0]=dbuf[1]=dbuf[2]=dbuf[3]=ledtable[LED_MINUS]; return; }
    bcd=(uint8_t)((tr>>8)&0x7F); dec=(uint8_t)(tr&0xFF);
    tens=(bcd>>4)&0x0F; units=bcd&0x0F;
    if(tr&0x8000) {
        dbuf[0]=ledtable[LED_MINUS];
        if(tens==0) {
            dbuf[1]=ledtable[units]|DP_BIT; dbuf[2]=ledtable[dec]; dbuf[3]=ledtable[LED_C];
        } else {
            dbuf[1]=ledtable[tens]; dbuf[2]=ledtable[units]; dbuf[3]=ledtable[LED_C];
        }
    } else {
        dbuf[0]=(tens==0)?ledtable[LED_BLANK]:ledtable[tens];
        dbuf[1]=ledtable[units]|DP_BIT;
        dbuf[2]=ledtable[dec];
        dbuf[3]=ledtable[LED_C];
    }
}


/* =======================================================================
   DS18B20
======================================================================= */

bit OW_Reset(void) {
    bit p;
    OW=0; DLY_LONG(300)
    OW=1; DLY70US;
    p=!OW; DLY_LONG(210)
    return p;
}

void OW_WriteByte(uint8_t b) {
    uint8_t i;
    for(i=0;i<8;i++) {
        if(b&0x01) { OW=0; DLY5US;  OW=1; DLY60US; }
        else        { OW=0; DLY60US; OW=1; DLY5US;  }
        b>>=1;
    }
}

uint8_t OW_ReadByte(void) {
    uint8_t i,v=0;
    for(i=0;i<8;i++) {
        v>>=1;
        OW=0; _nop_(); _nop_();
        OW=1; DLY10US;
        if(OW) v|=0x80;
        DLY60US;
    }
    return v;
}

uint16_t DS18B20_ReadTemp(void) {
    uint8_t lsb,msb,th;
    uint16_t raw;
    uint8_t t;

    if(!OW_Reset()) return 0xFFFF;
    OW_WriteByte(0xCC); OW_WriteByte(0x44);
    OW = 1;
    for(t=0; t<200; t++) {
        display_update();
        if(OW) break;
    }
    display_update();

    if(!OW_Reset()) return 0xFFFF;
    OW_WriteByte(0xCC); OW_WriteByte(0xBE);
    lsb=OW_ReadByte(); msb=OW_ReadByte();

    raw=((uint16_t)msb<<8)|lsb;
    if(raw&0x8000) {
        raw=(~raw)+1; th=(uint8_t)(raw>>4);
        return (uint16_t)(0x8000|((uint16_t)((th/10)*16+(th%10))<<8)|(uint8_t)((raw&0x0F)*10/16));
    }
    if(msb>0x04) return 0xFFFF;
    th=(uint8_t)(raw>>4);
    if(th>75) return 0xFFFF;
    return (uint16_t)(((uint16_t)((th/10)*16+(th%10))<<8)|(uint8_t)((raw&0x0F)*10/16));
}

/* =======================================================================
   TLACIDLA
======================================================================= */
void button_status(void) {
    if(P3_4==0) {
        if(debounce[0]<BUTTON_PRESS_LONG) {
            debounce[0]++;
            if(debounce[0]==BUTTON_PRESS)      { B1_RELEASED=0; B1_PRESSED=1; }
            if(debounce[0]==BUTTON_PRESS_LONG) { B1_PRESSED_LONG=1; }
        }
    } else {
        debounce[0]=0;
        if(B1_PRESSED) { B1_RELEASED=1; B1_PRESSED=0; }
    }
    if(P3_5==0) {
        if(debounce[1]<BUTTON_PRESS_LONG) {
            debounce[1]++;
            if(debounce[1]==BUTTON_PRESS)      { B2_RELEASED=0; B2_PRESSED=1; }
            if(debounce[1]==BUTTON_PRESS_LONG) { B2_RELEASED_LONG=0; B2_PRESSED_LONG=1; }
        }
    } else {
        debounce[1]=0;
        if(B2_PRESSED)      { B2_RELEASED=1;      B2_PRESSED=0; }
        if(B2_PRESSED_LONG) { B2_RELEASED_LONG=1; B2_PRESSED_LONG=0; }
    }
}

/* =======================================================================
   TIMER0 ISR
======================================================================= */
void timer0_isr(void) interrupt 1 using 1 {
    TL0=CLOCK_TIMER_LOW; TH0=CLOCK_TIMER_HIGH;

    if(clock_state==EDIT_HOUR||clock_state==EDIT_MIN) {
        if(++edit_blink_cnt>=6) { edit_blink_cnt=0; edit_blink=!edit_blink; }
        if(P3_5==0) { if(++repeat_tick>=5) { repeat_tick=0; clock_increment=1; } }
        else        { repeat_tick=0; clock_increment=0; }
    } else { edit_blink_cnt=0; edit_blink=1; repeat_tick=0; clock_increment=0; }

    if(clock_state!=TEMPERATURE) button_status();
}

/* =======================================================================
   INIT
======================================================================= */
void init(void) {
    P1=0x00; SR_CLK=0; SR_DATA=0; sr_all_off();
    TMOD=0x01; TH0=CLOCK_TIMER_HIGH; TL0=CLOCK_TIMER_LOW;
    PT0=1; ET0=1; TR0=1; EA=1;
    DS1302_CE=0; DS1302_SCLK=0; DS1302_IO=1;
    P3_4=1; OW=1;
}

/* =======================================================================
   MAIN
======================================================================= */
void main(void) {
    clock_state_t prev_state;
    uint8_t last_sec = 255;    /* posledna sekunda — detekcia zmeny */
    uint16_t temp = 0xFFFF;    /* posledna namerena teplota */
    uint16_t refresh = 50;     /* citac obnovy teploty */

    init();
    ds1302_init_default();

    while(1) {
        prev_state = clock_state;
        ds1302_read_time();                          /* nacitaj cas z RTC */

        /* blikanie dvojbodky synchronizovane so sekundami RTC */
        if(clock_second!=last_sec) { last_sec=clock_second; show_colon=!show_colon; }

        switch(clock_state) {

            case EDIT_MIN:
                /* pri vstupe do stavu inicializuj editacne hodnoty z RTC */
                if(prev_state!=EDIT_MIN) { edit_hour=clock_hour; edit_min=clock_minute; }
                if(B1_PRESSED) {                         /* potvrdit a ulozit */
                    clock_state=NORMAL; B1_PRESSED=0;
                    ds1302_write_time(edit_hour,edit_min);
                } else if(B2_PRESSED) {                  /* +1 minuta */
                    if(++edit_min==60) edit_min=0; B2_PRESSED=0;
                } else if(B2_PRESSED_LONG&&clock_increment) { /* rychle +1 */
                    if(++edit_min==60) edit_min=0; clock_increment=0;
                }
                /* hodiny stale, minuty blikaju */
                dbuf[0]=ledtable[edit_hour/10]; dbuf[1]=ledtable[edit_hour%10];
                if(edit_blink) { dbuf[2]=ledtable[edit_min/10]; dbuf[3]=ledtable[edit_min%10]; }
                else           { dbuf[2]=dbuf[3]=ledtable[LED_BLANK]; }
                break;

            case EDIT_HOUR:
                if(prev_state!=EDIT_HOUR) { edit_hour=clock_hour; edit_min=clock_minute; }
                if(B1_PRESSED) { clock_state=EDIT_MIN; B1_PRESSED=0; }  /* dalej na minuty */
                else if(B2_PRESSED) { if(++edit_hour==24) edit_hour=0; B2_PRESSED=0; }         /* +1 hodina */
                else if(B2_PRESSED_LONG&&clock_increment) { if(++edit_hour==24) edit_hour=0; clock_increment=0; } /* rychle +1 */
                /* hodiny blikaju, minuty stale */
                if(edit_blink) { dbuf[0]=ledtable[edit_hour/10]; dbuf[1]=ledtable[edit_hour%10]; }
                else           { dbuf[0]=dbuf[1]=ledtable[LED_BLANK]; }
                dbuf[2]=ledtable[edit_min/10]; dbuf[3]=ledtable[edit_min%10];
                break;

            case MIN_SEC:
                if(B2_RELEASED) { clock_state=NORMAL; B2_RELEASED=0; } /* spat na hodiny */
                /* zobraz minuty:sekundy */
                dbuf[0]=ledtable[clock_minute/10]; dbuf[1]=ledtable[clock_minute%10];
                dbuf[2]=ledtable[clock_second/10]; dbuf[3]=ledtable[clock_second%10];
                if(show_colon) dbuf[1]|=1;
                break;

            case TEMPERATURE:
                if(refresh>=150) {           /* spusti nove meranie každych ~150 cyklov */
                    refresh=0;
                    EA=0;                    /* zakaz prerusenia pocas 1-Wire komunikacie */
                    temp=DS18B20_ReadTemp();
                    EA=1; OW=1;             /* obnov linku po komunikacii */
                }
                refresh++;
                set_temp_dbuf(temp);        /* priprav zobrazenie teploty */
                /* jednoducha debounce BTN1 — button_status sa tu z ISR nevola */
                if(P3_4==0) {
                    if(++debounce[0]==BUTTON_PRESS) B1_PRESSED=1;
                } else {
                    if(B1_PRESSED) { clock_state=NORMAL; B1_PRESSED=0; } /* spat na hodiny */
                    debounce[0]=0;
                }
                break;

            case NORMAL:
            default:
                if(B2_RELEASED) { clock_state=MIN_SEC; B2_RELEASED=0; }    /* zobraz min:sek */
                else if(B1_PRESSED_LONG) {                                 /* vstup do editacie */
                    edit_hour=clock_hour; edit_min=clock_minute;
                    clock_state=EDIT_HOUR; B1_PRESSED=0; B1_PRESSED_LONG=0;
                }
                else if(B1_RELEASED) { clock_state=TEMPERATURE; B1_RELEASED=0; } /* zobraz teplotu */
                /* zobraz hodiny:minuty s blikajucou dvojbodkou */
                dbuf[0]=ledtable[clock_hour/10]; dbuf[1]=ledtable[clock_hour%10];
                dbuf[2]=ledtable[clock_minute/10]; dbuf[3]=ledtable[clock_minute%10];
                if(show_colon) dbuf[1]|=1;
                break;
        }

        display_update();   /* obnov displej (MUX 4 digity cez 74HC164) */
    }
}