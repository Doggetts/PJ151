#include "main.h"

unsigned char datt[] = "RUN!\r\n";     //上电串口提示信息
SYS_DATA_TYPE_STRUCT Sys;              //系统全局变量
COMx_Define COM1;                      //串口1通信变量

//========================================================================
//                            硬件引脚定义
//========================================================================
#define PWM_LED  P15                   //白光LED控制 PWM CH3
#define PWM_G    P11                   //绿光LED控制 PWM CH2
#define PWM_R    P13                   //红光LED控制 PWM CH1
#define LD_EN    P16                   //激光器控制 低电平开(硬件反相)
#define V5_EN    P17                   //5V升压使能 高电平开
#define TEMP     P10                   //NTC温度检测 ADC
#define USART_RX P30                   //串口接收
#define USART_TX P31                   //串口发送
#define KEY_RED    P37                 //红键  0:按下 1:松开
#define KEY_GREEN  P36                 //绿键  0:按下 1:松开
#define KEY_WEAK   P35                 //弱键  0:按下 1:松开
#define KEY_LEFT   P34                 //左档  0:按下 1:松开
#define KEY_MID    P32                 //中档  0:按下 1:松开
#define KEY_RIGHT  P33                 //右档  0:按下 1:松开

//========================================================================
//                            按键扫描结构体
//========================================================================
typedef struct
{
	unsigned char stable;              //消抖后稳定电平 0:按下 1:松开
	unsigned char pressed;             //当前是否处于按下状态
	unsigned int  debounce_ms;         //消抖累计时间
	unsigned int  hold_ms;             //本次按下持续时间
	unsigned char click_cnt;           //连击计数
	unsigned int  click_wait_ms;       //等待双击超时计数
	unsigned char long1_done;          //长按1s已触发标志
	unsigned char long10_done;         //长按10s已触发标志
	unsigned char dbl_wait;            //等待双击第二下标志
	unsigned char dbl_hold_arm;        //双击按住模式已激活
	unsigned int  second_hold_ms;      //双击第二下按住时间
} KEY_SCAN_TYPE;

static KEY_SCAN_TYPE KeyRed, KeyGreen, KeyWeak;  //三颗功能键扫描状态

//========================================================================
// 函数: Key_Read
// 描述: 读取按键逻辑电平(高电平=松开).
// 参数: pin - 引脚电平.
// 返回: 1松开 0按下.
//========================================================================
static unsigned char Key_Read(unsigned char pin)
{
	return pin ? 1 : 0;
}

//========================================================================
// 函数: Gear_Get
// 描述: 读取当前档位(优先级:中>右>左, 中档引脚有效时一律为中档).
// 参数: None.
// 返回: 档位编号 GEAR_xxx.
//========================================================================
static unsigned char Gear_Get(void)
{
	if(!Key_Read(KEY_MID))   return GEAR_MID;
	if(!Key_Read(KEY_RIGHT)) return GEAR_RIGHT;
	if(!Key_Read(KEY_LEFT))  return GEAR_LEFT;
	return GEAR_NONE;
}

//========================================================================
// 函数: Output_AllOff
// 描述: 关闭所有光源输出并复位模式.
// 参数: None.
// 返回: None.
//========================================================================
void Output_AllOff(void)
{
	Sys.Out_White = 0;
	Sys.Out_Green = 0;
	Sys.Out_Red = 0;
	Sys.Out_Laser = 0;
	Sys.Strobe_On = 0;
	Sys.Moment_Active = 0;
	Sys.Light_Mode = MODE_OFF;
}

//========================================================================
// 函数: Status_Report
// 描述: 主动上报完整状态(同<>A5查询回复).
// 参数: None.
// 返回: None.
//========================================================================
void Status_Report(void)
{
	printf("Sta=%bu,%bu,%bu,%bu,%bu,%bu,%bu\r\n",
		Sys.Light_Mode, Sys.Gear, Sys.V5_On,
		Sys.Out_White, Sys.Out_Green, Sys.Out_Red, Sys.Out_Laser);
}

static void Status_PollReport(void)
{
	static unsigned char last_mode = 0xFF;
	static unsigned char last_gear = 0xFF;
	static unsigned char last_white = 0xFF;
	static unsigned char last_green = 0xFF;
	static unsigned char last_red = 0xFF;
	static unsigned char last_laser = 0xFF;

	if(last_mode != Sys.Light_Mode ||
		last_gear != Sys.Gear ||
		last_white != Sys.Out_White ||
		last_green != Sys.Out_Green ||
		last_red != Sys.Out_Red ||
		last_laser != Sys.Out_Laser)
	{
		last_mode = Sys.Light_Mode;
		last_gear = Sys.Gear;
		last_white = Sys.Out_White;
		last_green = Sys.Out_Green;
		last_red = Sys.Out_Red;
		last_laser = Sys.Out_Laser;
		Status_Report();
	}
}

//========================================================================
// 函数: Battery_Check
// 描述: 电池电压检测, <3V串口报警.
// 参数: None.
// 返回: None.
//========================================================================
static void Battery_Check(void)
{
	static unsigned char alarm_cnt = 0;
	float v = Sys.Vref_Voltage;

	if(v < BAT_VOLT_ALARM)
	{
		if(++alarm_cnt >= BAT_VOLT_DEBOUNCE)
		{
			if(!Sys.Bat_Alarm)
			{
				Sys.Bat_Alarm = 1;
				printf("Alarm=LowVolt\r\n");
			}
		}
	}
	else
	{
		alarm_cnt = 0;
		if(v > BAT_VOLT_ALARM_CLR)
			Sys.Bat_Alarm = 0;
	}
}

//========================================================================
// 函数: Mode_StartBlocked
// 描述: 当前无出光时, 电池电压过低则禁止新开启模式.
// 参数: None.
// 返回: 1禁止开启 0允许.
//========================================================================
static unsigned char Mode_StartBlocked(void)
{
	if(Sys.Light_Mode != MODE_OFF)
		return 0;
	if(Sys.Out_Laser || Sys.Out_White || Sys.Out_Green || Sys.Out_Red)
		return 0;
	if(Sys.Vref_Voltage < BAT_VOLT_MODE_BLOCK)
		return 1;
	return 0;
}

//========================================================================
// 函数: Mode_Set
// 描述: 切换到指定常亮模式.
// 参数: mode - 出光模式编号.
// 返回: None.
//========================================================================
void Mode_Set(unsigned char mode)
{
	if(mode != MODE_OFF && Mode_StartBlocked())
		return;

	Output_AllOff();
	Sys.Light_Mode = mode;

	switch(mode)
	{
		case MODE_LASER_HOLD:           //激光常亮
			Sys.Out_Laser = 1;
			break;
		case MODE_LASER_WHITE_HOLD:     //激光+白光常亮
			Sys.Out_Laser = 1;
			Sys.Out_White = 1;
			break;
		case MODE_WHITE_HOLD:           //白光常亮
			Sys.Out_White = 1;
			break;
		case MODE_WHITE_STROBE_HOLD:    //白光8Hz眩目常亮
			Sys.Out_White = 1;
			Sys.Strobe_On = 1;
			break;
		case MODE_WHITE_DIM_HOLD:       //弱白光常亮
			Sys.Out_White = 2;
			break;
		case MODE_RED_HOLD:             //红光常亮
			Sys.Out_Red = 1;
			break;
		case MODE_GREEN_HOLD:           //绿光常亮
			Sys.Out_Green = 1;
			break;
		default:
			break;
	}
}

//========================================================================
// 函数: Mode_Toggle
// 描述: 常亮模式切换(同模式再触发则关闭).
// 参数: mode - 出光模式编号.
// 返回: None.
//========================================================================
static void Mode_Toggle(unsigned char mode)
{
	if(Sys.Light_Mode == mode)
		Output_AllOff();
	else
		Mode_Set(mode);
}

//========================================================================
// 函数: Output_Apply
// 描述: 根据系统变量刷新硬件输出(PWM/激光/5V使能).
// 参数: None.
// 返回: None.
//========================================================================
void Output_Apply(void)
{
	unsigned char white_on = 0;
	unsigned char white_dim = 0;

	/* 白光输出处理(含8Hz爆闪) */
	if(Sys.Out_White == 1)
	{
		if(Sys.Strobe_On)
			white_on = Sys.Strobe_Phase;
		else
			white_on = 1;
	}
	else if(Sys.Out_White == 2)
	{
		if(Sys.Strobe_On)
			white_dim = Sys.Strobe_Phase;
		else
			white_dim = 1;
	}

	/* 有任意光源输出时开启5V升压 */
	if(Sys.Out_Laser || Sys.Out_White || Sys.Out_Green || Sys.Out_Red)
	{
		V5_EN = 1;
		Sys.V5_On = 1;
	}
	else
	{
		V5_EN = 0;
		Sys.V5_On = 0;
	}

	LD_EN = Sys.Out_Laser ? 0 : 1;      //低电平开启激光

	/* PWM占空比输出 */
	if(white_on)
		PWM_SetWhite(PWM_DUTY_FULL);
	else if(white_dim)
		PWM_SetWhite(PWM_DUTY_DIM);
	else
		PWM_SetWhite(0);

	if(Sys.Out_Green)
		PWM_SetGreen(PWM_DUTY_FULL);
	else
		PWM_SetGreen(0);

	if(Sys.Out_Red)
		PWM_SetRed(PWM_DUTY_FULL);
	else
		PWM_SetRed(0);
}

//========================================================================
// 函数: App_Drive_Task
// 描述: 输出驱动任务,周期20ms.
// 参数: None.
// 返回: None.
//========================================================================
void App_Drive_Task(void)
{
	Output_Apply();
	Status_PollReport();
}

//========================================================================
// 函数: Key_Reset
// 描述: 复位单颗按键的扫描状态机.
// 参数: k - 按键扫描结构体指针.
// 返回: None.
//========================================================================
static void Key_Reset(KEY_SCAN_TYPE *k)
{
	k->hold_ms = 0;
	k->click_cnt = 0;
	k->click_wait_ms = 0;
	k->long1_done = 0;
	k->long10_done = 0;
	k->dbl_wait = 0;
	k->dbl_hold_arm = 0;
	k->second_hold_ms = 0;
	k->pressed = 0;
}

//========================================================================
// 函数: Key_OnLong1
// 描述: 长按1s触发(右档激光/白光瞬亮).
// 参数: k - 按键状态; gear - 档位; key_id - 1红2绿3弱.
// 返回: None.
//========================================================================
static void Key_OnLong1(KEY_SCAN_TYPE *k, unsigned char gear, unsigned char key_id)
{
	if(k->long1_done) return;
	if(Mode_StartBlocked()) return;
	k->long1_done = 1;
	k->click_cnt = 0;
	k->click_wait_ms = 0;
	k->dbl_wait = 0;

	if(gear == GEAR_RIGHT)
	{
		if(key_id == 2)                 //右档+绿键 激光瞬亮
		{
			Sys.Light_Mode = MODE_LASER_MOMENT;
			Sys.Out_Laser = 1;
			Sys.Moment_Active = 1;
		}
		else if(key_id == 1)            //右档+红键 白光瞬亮
		{
			Sys.Light_Mode = MODE_WHITE_MOMENT;
			Sys.Out_White = 1;
			Sys.Moment_Active = 1;
		}
	}
}

//========================================================================
// 函数: Key_OnLong10
// 描述: 长按10s触发(左档红/绿光常亮锁定).
// 参数: k - 按键状态; gear - 档位; key_id - 1红2绿3弱.
// 返回: None.
//========================================================================
static void Key_OnLong10(KEY_SCAN_TYPE *k, unsigned char gear, unsigned char key_id)
{
	if(k->long10_done) return;
	k->long10_done = 1;
	k->click_cnt = 0;
	k->click_wait_ms = 0;

	if(gear == GEAR_LEFT)
	{
		if(key_id == 1)                 //左档+红键 红光常亮
			Mode_Set(MODE_RED_HOLD);
		else if(key_id == 2)            //左档+绿键 绿光常亮
			Mode_Set(MODE_GREEN_HOLD);
	}
}

//========================================================================
// 函数: Key_OnSingleClick
// 描述: 单击触发(各档位短按功能).
// 参数: gear - 档位; key_id - 1红2绿3弱.
// 返回: None.
//========================================================================
static void Key_OnSingleClick(unsigned char gear, unsigned char key_id)
{
	if(gear == GEAR_RIGHT)
	{
		if(key_id == 2)                 //右档+绿键 激光常亮
		{
			if(Sys.Light_Mode == MODE_LASER_WHITE_HOLD)
				Output_AllOff();
			else
				Mode_Toggle(MODE_LASER_HOLD);
		}
		else if(key_id == 1)            //右档+红键 白光常亮
		{
			if(Sys.Light_Mode == MODE_WHITE_STROBE_HOLD)
				Output_AllOff();
			else
				Mode_Toggle(MODE_WHITE_HOLD);
		}
		else if(key_id == 3)            //右档+弱键 弱白光常亮
			Mode_Toggle(MODE_WHITE_DIM_HOLD);
	}
	else if(gear == GEAR_LEFT)
	{
		if(key_id == 3)                 //左档+弱键 弱白光常亮
			Mode_Toggle(MODE_WHITE_DIM_HOLD);
	}
}

//========================================================================
// 函数: Key_OnDoubleClick
// 描述: 双击触发(右档绿/红键组合功能).
// 参数: gear - 档位; key_id - 1红2绿3弱.
// 返回: None.
//========================================================================
static void Key_OnDoubleClick(unsigned char gear, unsigned char key_id)
{
	if(gear == GEAR_RIGHT)
	{
		if(key_id == 2)                 //右档+绿键 激光+白光常亮
			Mode_Toggle(MODE_LASER_WHITE_HOLD);
		else if(key_id == 1)            //右档+红键 白光8Hz眩目常亮
			Mode_Toggle(MODE_WHITE_STROBE_HOLD);
	}
}

//========================================================================
// 函数: Key_OnDoubleHold
// 描述: 双击后第二下按住触发(瞬亮模式).
// 参数: gear - 档位; key_id - 1红2绿3弱.
// 返回: None.
//========================================================================
static void Key_OnDoubleHold(unsigned char gear, unsigned char key_id)
{
	if(Mode_StartBlocked()) return;
	Output_AllOff();
	if(gear == GEAR_RIGHT)
	{
		if(key_id == 2)                 //右档+绿键 激光+白光瞬亮
		{
			Sys.Light_Mode = MODE_LASER_WHITE_MOMENT;
			Sys.Out_Laser = 1;
			Sys.Out_White = 1;
			Sys.Moment_Active = 1;
		}
		else if(key_id == 1)            //右档+红键 白光8Hz眩目瞬亮
		{
			Sys.Light_Mode = MODE_WHITE_STROBE_MOMENT;
			Sys.Out_White = 1;
			Sys.Strobe_On = 1;
			Sys.Moment_Active = 1;
		}
	}
}

//========================================================================
// 函数: Key_OnRelease
// 描述: 按键松开事件处理.
// 参数: k - 按键状态; gear - 档位; key_id - 1红2绿3弱.
// 返回: None.
//========================================================================
static void Key_OnRelease(KEY_SCAN_TYPE *k, unsigned char gear, unsigned char key_id)
{
	if(k->long10_done)                  //长按10s锁定后松开,保持常亮
	{
		Key_Reset(k);
		return;
	}

	if(k->long1_done)                   //长按1s瞬亮,松开关闭
	{
		if(Sys.Moment_Active)
			Output_AllOff();
		Key_Reset(k);
		return;
	}

	if(gear == GEAR_LEFT && !k->long10_done)
	{
		if(key_id == 1 && Sys.Light_Mode == MODE_RED_MOMENT)    //左档红光瞬亮松开
			Output_AllOff();
		else if(key_id == 2 && Sys.Light_Mode == MODE_GREEN_MOMENT) //左档绿光瞬亮松开
			Output_AllOff();
	}

	if(k->dbl_hold_arm)                 //双击按住瞬亮,松开关闭
	{
		if(Sys.Moment_Active)
			Output_AllOff();
		Key_Reset(k);
		return;
	}

	if(k->dbl_wait)                     //双击第二下快速松开=双击常亮
	{
		k->dbl_wait = 0;
		if(k->second_hold_ms <= KEY_DBLHOLD_MS)
			Key_OnDoubleClick(gear, key_id);
		Key_Reset(k);
		return;
	}

	if(k->click_cnt == 1)
		k->click_wait_ms = KEY_DBLCLICK_MS;     //等待可能的双击
	else if(k->click_cnt >= 2)
	{
		Key_OnDoubleClick(gear, key_id);
		Key_Reset(k);
	}
}

//========================================================================
// 函数: Key_Process
// 描述: 单颗按键扫描与手势识别(消抖/单击/双击/长按).
// 参数: k - 按键状态; raw - 原始电平; gear - 档位; key_id - 1红2绿3弱.
// 返回: None.
//========================================================================
static void Key_Process(KEY_SCAN_TYPE *k, unsigned char raw, unsigned char gear, unsigned char key_id)
{
	/* 电平变化消抖 */
	if(raw != k->stable)
	{
		k->debounce_ms += 20;
		if(k->debounce_ms >= KEY_DEBOUNCE_MS)
		{
			k->stable = raw;
			k->debounce_ms = 0;

			if(k->stable == 0)          //按下沿
			{
				k->pressed = 1;
				k->hold_ms = 0;
				if(k->click_wait_ms > 0 && k->click_cnt == 1)   //双击第二下
				{
					k->click_cnt = 2;
					k->click_wait_ms = 0;
					k->dbl_wait = 1;
					k->second_hold_ms = 0;
				}
			}
			else                        //松开沿
			{
				if(k->pressed)
				{
					if(!k->long1_done && !k->long10_done && !k->dbl_wait)
						k->click_cnt++;
					Key_OnRelease(k, gear, key_id);
				}
				k->pressed = 0;
			}
		}
	}
	else
		k->debounce_ms = 0;

	/* 按下保持中 */
	if(k->pressed && k->stable == 0)
	{
		if(k->hold_ms < 60000)
			k->hold_ms += 20;

		/* 双击第二下按住判定 */
		if(k->dbl_wait)
		{
			if(k->second_hold_ms < 60000)
				k->second_hold_ms += 20;
			if(k->second_hold_ms > KEY_DBLHOLD_MS)
			{
				k->dbl_hold_arm = 1;
				Key_OnDoubleHold(gear, key_id);
			}
		}

		/* 右档长按1s */
		if(gear == GEAR_RIGHT && (key_id == 1 || key_id == 2))
		{
			if(k->hold_ms >= KEY_LONG1_MS)
				Key_OnLong1(k, gear, key_id);
		}

		/* 左档红/绿键处理 */
		if(gear == GEAR_LEFT && (key_id == 1 || key_id == 2))
		{
			if(k->hold_ms >= KEY_LONG10_MS)         //长按10s锁定常亮
				Key_OnLong10(k, gear, key_id);
			else if(!k->long10_done)
			{
				if(key_id == 1 && Sys.Light_Mode != MODE_RED_HOLD)
				{
					if(Mode_StartBlocked()) return;
					Sys.Light_Mode = MODE_RED_MOMENT;
					Sys.Out_Red = 1;
					Sys.Moment_Active = 1;
				}
				else if(key_id == 2 && Sys.Light_Mode != MODE_GREEN_HOLD)
				{
					if(Mode_StartBlocked()) return;
					Sys.Light_Mode = MODE_GREEN_MOMENT;
					Sys.Out_Green = 1;
					Sys.Moment_Active = 1;
				}
			}
		}
	}

	/* 单击等待超时,判定为单次点击 */
	if(k->click_wait_ms > 0)
	{
		if(k->click_wait_ms > 20)
			k->click_wait_ms -= 20;
		else
		{
			if(k->click_cnt == 1)
			{
				/* 左档红/绿常亮模式下短按关闭 */
				if(gear == GEAR_LEFT && key_id == 1 && Sys.Light_Mode == MODE_RED_HOLD)
					Output_AllOff();
				else if(gear == GEAR_LEFT && key_id == 2 && Sys.Light_Mode == MODE_GREEN_HOLD)
					Output_AllOff();
				else
					Key_OnSingleClick(gear, key_id);
			}
			Key_Reset(k);
		}
	}
}

//========================================================================
// 函数: App_Key_Task
// 描述: 按键处理任务,周期20ms.
// 参数: None.
// 返回: None.
//========================================================================
void App_Key_Task(void)
{
	unsigned char gear;
	static unsigned char gear_last = 0xFF;

	gear = Gear_Get();

	/* 任意档位改变则关闭输出并复位按键状态 */
	if(gear != gear_last)
	{
		if(gear_last != 0xFF)
		{
			Output_AllOff();
			Output_Apply();
			Key_Reset(&KeyRed);
			Key_Reset(&KeyGreen);
			Key_Reset(&KeyWeak);
			Sys.MidHold_ms = 0;
		}
		gear_last = gear;
	}
	Sys.Gear = gear;

	if(gear == GEAR_RIGHT || gear == GEAR_LEFT)
	{
		Key_Process(&KeyRed,   Key_Read(KEY_RED),   gear, 1);
		Key_Process(&KeyGreen, Key_Read(KEY_GREEN), gear, 2);
		Key_Process(&KeyWeak,  Key_Read(KEY_WEAK),  gear, 3);
	}
	else
	{
		Key_Reset(&KeyRed);
		Key_Reset(&KeyGreen);
		Key_Reset(&KeyWeak);
	}

	/* 中档保持5s后进入休眠, P32上升沿唤醒 */
	if(Sys.Gear == GEAR_MID)
	{
		if(Sys.MidHold_ms < 65000)
			Sys.MidHold_ms += 20;
		if(Sys.MidHold_ms >= MID_SHUTDOWN_MS)
		{
			Sys.MidHold_ms = 0;
			ShutDown();
		}
	}
	else
		Sys.MidHold_ms = 0;
}

//========================================================================
// 函数: Usart_HandleCmd
// 描述: 串口命令解析(帧格式: <>命令).
// 参数: None.
// 返回: None.
//
// 查询命令(C51须用%bu打印unsigned char, 否则参数栈错位):
//   <A1> 版本号    <A2> 温度    <A3> 电池电压
//   <A4> 当前模式  <A5> 完整状态
// 控制命令:
//   <M00>~<M13> 设置模式  <B0> 全关
//   <C1=0/1> 激光  <C2=0/1> 白光强  <C3=0/1> 白光弱
//   <C4=0/1> 绿光  <C5=0/1> 红光  <D1=0/1> 爆闪
//   <>REBOOT 重启
//========================================================================
static void Usart_HandleCmd(void)
{
	char *p = COM1.RX_Buffer;

	if(p[0] != '<' || p[1] != '>') return;

	/* A类: 查询命令 */
	if(p[2] == 'A')
	{
		if(p[3] == '1')
			printf("Ver=%bu.%bu\r\n", Sys.Ver_Major, Sys.Ver_Minor);
		else if(p[3] == '2')
			printf("Temp=%.1f\r\n", Sys.Board_Temp);
		else if(p[3] == '3')
			printf("Vref=%.2f\r\n", Sys.Vref_Voltage);
		else if(p[3] == '4')
			printf("Mode=%bu\r\n", Sys.Light_Mode);
		else if(p[3] == '5')
			Status_Report();
		return;
	}

	/* M类: 模式设置 <M00>~<M13> */
	if(p[2] == 'M' && p[3] >= '0' && p[3] <= '9')
	{
		unsigned char mode = (p[3] - '0');
		if(p[4] >= '0' && p[4] <= '9')
			mode = mode * 10 + (p[4] - '0');
		if(mode <= MODE_GREEN_HOLD)
		{
			if(mode == MODE_OFF)
			{
				Output_AllOff();
				printf("OK\r\n");
			}
			else if(Mode_StartBlocked())
				printf("ERR\r\n");
			else
			{
				Mode_Set(mode);
				printf("OK\r\n");
			}
		}
		else
			printf("ERR\r\n");
		return;
	}

	/* B类: 全关 */
	if(p[2] == 'B' && p[3] == '0')
	{
		Output_AllOff();
		printf("OK\r\n");
		return;
	}

	/* C类: 独立通道控制 <Cx=0/1> */
	if(p[2] == 'C' && p[4] == '=' && (p[5] == '0' || p[5] == '1'))
	{
		unsigned char on = (p[5] == '1');
		if(on && Mode_StartBlocked())
		{
			printf("ERR\r\n");
			return;
		}
		if(p[3] == '1') { Sys.Out_Laser = on; Sys.Light_Mode = on ? MODE_LASER_HOLD : MODE_OFF; }
		else if(p[3] == '2') { Sys.Out_White = on ? 1 : 0; if(on) Sys.Light_Mode = MODE_WHITE_HOLD; }
		else if(p[3] == '3') { Sys.Out_White = on ? 2 : 0; if(on) Sys.Light_Mode = MODE_WHITE_DIM_HOLD; }
		else if(p[3] == '4') { Sys.Out_Green = on; if(on) Sys.Light_Mode = MODE_GREEN_HOLD; }
		else if(p[3] == '5') { Sys.Out_Red = on; if(on) Sys.Light_Mode = MODE_RED_HOLD; }
		else { printf("ERR\r\n"); return; }
		if(!on && !Sys.Out_Laser && !Sys.Out_White && !Sys.Out_Green && !Sys.Out_Red)
			Sys.Light_Mode = MODE_OFF;
		printf("OK\r\n");
		return;
	}

	/* D类: 爆闪控制 <D1=0/1> */
	if(p[2] == 'D' && p[3] == '1' && p[4] == '=' && (p[5] == '0' || p[5] == '1'))
	{
		unsigned char on = (p[5] == '1');
		if(on && Sys.Out_White == 0 && Mode_StartBlocked())
		{
			printf("ERR\r\n");
			return;
		}
		Sys.Strobe_On = on;
		if(Sys.Strobe_On && Sys.Out_White == 0)
			Sys.Out_White = 1;
		printf("OK\r\n");
		return;
	}

	/* 重启 */
	if(p[2] == 'R' && p[3] == 'E' && p[4] == 'B' && p[5] == 'O' && p[6] == 'O' && p[7] == 'T')
	{
		IAP_CONTR |= 0x60;
		return;
	}

	printf("ERR\r\n");
}

//========================================================================
// 函数: App_Usart_Task
// 描述: 串口接收任务,帧超时后解析,周期16ms.
// 参数: None.
// 返回: None.
//========================================================================
void App_Usart_Task(void)
{
	if(COM1.RX_TimeOut > 0)
	{
		if(--COM1.RX_TimeOut == 0)
		{
			Usart_HandleCmd();
			COM1.RX_Cnt = 0;
		}
	}
}

//========================================================================
// 函数: App_Analysis_Task
// 描述: ADC采集与NTC温度计算,周期500ms.
// 参数: None.
// 返回: None.
//========================================================================
void App_Analysis_Task(void)
{
	unsigned int AD_Board_Temp, AD_Vref_Voltage;
	float T2 = 298.15;                  //参考温度25℃(K)
	float Ka = 273.15;
	float V_adc, R_ntc;
	int *BGV;

	BGV = (int idata *)0xef;

	AD_Vref_Voltage = ADCRead(15);
	AD_Board_Temp = ADCRead(0);

	Sys.Vref_Voltage = 1.024 * *BGV / AD_Vref_Voltage;

	/*
	 * 分压电路: Vcc--[NTC]--P10(ADC)--[10K]--GND  (NTC在高端)
	 * V_adc = Vcc * Rpull / (Rntc + Rpull)
	 * Rntc  = Rpull * (Vcc - V_adc) / V_adc
	 */
	V_adc = (float)AD_Board_Temp / ADC_FULL_SCALE * NTC_SUPPLY_V;
	if(V_adc < 0.01f)
		V_adc = 0.01f;
	R_ntc = NTC_R_PULL * (NTC_SUPPLY_V - V_adc) / V_adc;

	Sys.Board_Temp = 1.0f / (1.0f / T2 + log(R_ntc / NTC_R25) / NTC_B_VALUE) - Ka + 0.5f;

	/* 温度限幅 */
	if(Sys.Board_Temp < -40)
		Sys.Board_Temp = -40;
	else if(Sys.Board_Temp > 120)
		Sys.Board_Temp = 120;

	Battery_Check();
}

//========================================================================
//                            任务调度表
//========================================================================
static TASK_COMPONENTS Task_Comps[] =
{
//状态  计数  周期  函数
	{0, 20,  20,  App_Key_Task},        /* 按键处理     Period: 20ms  */
	{0, 500, 500, App_Analysis_Task},   /* 信号采集     Period: 500ms */
	{0, 20,  20,  App_Drive_Task},      /* 输出驱动     Period: 20ms  */
	{0, 16,  16,  App_Usart_Task},      /* 串口解析     Period: 16ms  */
};

unsigned char Tasks_Max = sizeof(Task_Comps) / sizeof(Task_Comps[0]);

//========================================================================
// INT0中断  中档P32唤醒后软复位开机
//========================================================================
void INT0_Isr() interrupt 0
{
	Hw_IO_Init();
	IAP_CONTR |= 0x60;                  //软复位,重新从main启动
}

//========================================================================
// 定时器0中断  系统时钟  每1ms中断1次
//========================================================================
void TM0_Isr() interrupt 1
{
	Sys.Tick_ms++;
	/* 8Hz爆闪: 周期125ms, 半周期约63ms */
	if(Sys.Strobe_On)
	{
		if((Sys.Tick_ms % 63) == 0)
			Sys.Strobe_Phase ^= 1;
	}
	else
		Sys.Strobe_Phase = 1;

	Task_Marks_Handler_Callback();
}

//========================================================================
// 串口中断
//========================================================================
void UartIsr() interrupt 4
{
	if(TI)                              //发送完成
	{
		TI = 0;
		COM1.busy = 0;
	}
	if(RI)                              //接收数据
	{
		RI = 0;
		if(COM1.busy == 0)
		{
			if(COM1.RX_Cnt >= COM_RX_Lenth)
				COM1.RX_Cnt = 0;
			COM1.RX_Buffer[COM1.RX_Cnt++] = SBUF;
			COM1.RX_TimeOut = TimeOutSet1;
		}
	}
}

//========================================================================
// 函数: main
// 描述: 主函数,初始化硬件并进入任务调度循环.
//========================================================================
void main()
{
	Hw_IO_Init();

	Data_Init();
	AdcInit();
	UartInit();
	PWM_Init();
	Timer0_Init();
	EA = 1;
	UartSendStr(datt);

	while(1)
		Task_Pro_Handler_Callback();
}

//========================================================================
// 函数: Task_Marks_Handler_Callback
// 描述: 任务标记回调,在1ms定时中断中递减任务计数器.
// 参数: None.
// 返回: None.
//========================================================================
void Task_Marks_Handler_Callback(void)
{
	unsigned char i;
	for(i = 0; i < Tasks_Max; i++)
	{
		if(Task_Comps[i].TIMCount)
		{
			Task_Comps[i].TIMCount--;
			if(Task_Comps[i].TIMCount == 0)
			{
				Task_Comps[i].TIMCount = Task_Comps[i].TRITime;
				Task_Comps[i].Run = 1;
			}
		}
	}
}

//========================================================================
// 函数: Task_Pro_Handler_Callback
// 描述: 任务处理回调,在主循环中执行已到时的任务.
// 参数: None.
// 返回: None.
//========================================================================
void Task_Pro_Handler_Callback(void)
{
	unsigned char i;
	for(i = 0; i < Tasks_Max; i++)
	{
		if(Task_Comps[i].Run)
		{
			Task_Comps[i].Run = 0;
			Task_Comps[i].TaskHook();
		}
	}
}

//========================================================================
// 函数: Data_Init
// 描述: 系统数据与按键状态初始化.
// 参数: None.
// 返回: None.
//========================================================================
void Data_Init(void)
{
	Output_AllOff();
	Sys.Gear = GEAR_NONE;
	Sys.Strobe_Phase = 1;
	Sys.Ver_Major = 1;
	Sys.Ver_Minor = 0;
	Sys.Bat_Alarm = 0;
	Sys.MidHold_ms = 0;
	KeyRed.stable = 1;                  //初始为松开状态
	KeyGreen.stable = 1;
	KeyWeak.stable = 1;
	Key_Reset(&KeyRed);
	Key_Reset(&KeyGreen);
	Key_Reset(&KeyWeak);
}

//========================================================================
// 函数: Timer0_Init
// 描述: 定时器0初始化,1ms中断@24MHz.
// 参数: None.
// 返回: None.
//========================================================================
void Timer0_Init(void)
{
	AUXR |= 0x80;                       //定时器时钟1T模式
	TMOD &= 0xF0;
	TL0 = 0x40;
	TH0 = 0xA2;
	TF0 = 0;
	TR0 = 1;
	ET0 = 1;
}

//========================================================================
// 函数: UartInit
// 描述: 串口1初始化,115200波特率,定时器1作波特率发生器.
// 参数: None.
// 返回: None.
//========================================================================
void UartInit()
{
	SCON = 0x50;                        //8位数据,可变波特率
	AUXR |= 0x40;                       //定时器时钟1T模式
	AUXR &= 0xFE;                       //串口1选择定时器1
	TMOD &= 0x0F;
	TL1 = BRT;
	TH1 = BRT >> 8;
	ET1 = 0;
	TR1 = 1;
	ES = 1;

	COM1.RX_Cnt = 0;
	COM1.RX_TimeOut = 0;
	COM1.busy = 0;
}

//========================================================================
// 函数: AdcInit
// 描述: ADC模块初始化.
// 参数: None.
// 返回: None.
//========================================================================
void AdcInit()
{
	P_SW2 |= 0x80;
	ADCTIM = 0x3f;                      //ADC内部时序
	P_SW2 &= 0x7f;
	ADCCFG = 0x24;
	ADC_CONTR |= 0x80;                  //使能ADC模块
}

//========================================================================
// 函数: PWM_Init
// 描述: PWMA初始化,配置绿/红/白三路PWM.
//       CH1-CCR1-P13红光  CH2-CCR2-P11绿光  CH3-CCR3-P15白光
// 参数: None.
// 返回: None.
//========================================================================
void PWM_Init(void)
{
	P_SW2 |= 0x80;
	PWMA_CCER1 = 0x00;                  //写CCMR前须先关闭通道
	PWMA_CCER2 = 0x00;

	PWMA_CCMR1 = 0x60;                  //CH1 PWMA输出 P13红光
	PWMA_CCMR2 = 0x60;                  //CH2 PWMA输出 P11绿光
	PWMA_CCMR3 = 0x60;                  //CH3 PWMA输出 P15白光

	PWMA_CCER1 = 0x44;                  //使能CH1 CH2
	PWMA_CCER2 = 0x04;                  //使能CH3

	PWMA_PSCR = 240 - 1;
	PWMA_ARR = PWM_ARR_MAX;
	PWMA_CCR1 = 0;
	PWMA_CCR2 = 0;
	PWMA_CCR3 = 0;

	PWMA_ENO = 0x2A;                    //使能CH1~CH3端口输出
	PWMA_BKR = 0x80;                    //使能主输出
	PWMA_CR1 = 0x01;                    //开始计时
	P_SW2 &= 0x7f;
}

//========================================================================
// 函数: PWM_SetWhite / PWM_SetGreen / PWM_SetRed
// 描述: 设置各通道PWM占空比,0=关闭.
// 参数: duty - 占空比计数值(0~PWM_ARR_MAX).
// 返回: None.
//========================================================================
void PWM_SetWhite(unsigned char duty)
{
	if(duty > PWM_ARR_MAX) duty = PWM_ARR_MAX;
	P_SW2 |= 0x80;
	PWMA_CCR3 = duty;                   //CH3 白光 P15
	P_SW2 &= 0x7f;
}

void PWM_SetGreen(unsigned char duty)
{
	if(duty > PWM_ARR_MAX) duty = PWM_ARR_MAX;
	P_SW2 |= 0x80;
	PWMA_CCR2 = duty;                   //CH2 绿光 P11
	P_SW2 &= 0x7f;
}

void PWM_SetRed(unsigned char duty)
{
	if(duty > PWM_ARR_MAX) duty = PWM_ARR_MAX;
	P_SW2 |= 0x80;
	PWMA_CCR1 = duty;                   //CH1 红光 P13
	P_SW2 &= 0x7f;
}

//========================================================================
// 函数: PWM_AllOff
// 描述: 关闭所有PWM通道.
// 参数: None.
// 返回: None.
//========================================================================
void PWM_AllOff(void)
{
	PWM_SetWhite(0);
	PWM_SetGreen(0);
	PWM_SetRed(0);
}

//========================================================================
// 函数: putchar
// 描述: printf重定向到串口1.
//========================================================================
char putchar(char c)
{
	UartSend(c);
	return c;
}

//========================================================================
// 函数: UartSend / UartSendStr
// 描述: 串口发送单字节/字符串.
//========================================================================
void UartSend(unsigned char dat)
{
	while(COM1.busy);
	COM1.busy = 1;
	SBUF = dat;
}

void UartSendStr(char *p)
{
	while(*p)
		UartSend(*p++);
}

//========================================================================
// 函数: ADCRead
// 描述: 查询法读取一次ADC转换结果.
// 参数: ChX - ADC通道号(0~15).
// 返回: ADC结果,失败返回4096.
//========================================================================
unsigned int ADCRead(unsigned char ChX)
{
	unsigned int res;
	unsigned char i;

	if(ChX > 15) return 4096;
	ADC_RES = 0;
	ADC_RESL = 0;
	ADC_CONTR = (ADC_CONTR & 0xf0) | 0x40 | ChX;
	_nop_();
	_nop_();

	for(i = 0; i < 250; i++)
	{
		if(ADC_CONTR & 0x20)
		{
			ADC_CONTR &= ~0x20;
			res = ((unsigned int)ADC_RES << 8) | ADC_RESL;
			return res;
		}
	}
	return 4096;
}

//========================================================================
// 函数: Hw_IO_Init
// 描述: 硬件IO口初始化(上电/唤醒复位后调用).
// 参数: None.
// 返回: None.
//========================================================================
void Hw_IO_Init(void)
{
	P1M0 = 0xea; P1M1 = 0x15;          //P10高阻 P11/P13/P15/P16/P17推挽
	P3M0 = 0x00; P3M1 = 0x00;          //P3全部准双向(按键+串口)
	P0M0 = 0x00; P0M1 = 0xff;          //P0全部高阻
	P2M0 = 0x00; P2M1 = 0xff;          //P2全部高阻
	P4M0 = 0x00; P4M1 = 0xff;          //P4全部高阻
	P5M0 = 0x00; P5M1 = 0xff;          //P5全部高阻

	LD_EN = 1;                          //默认关闭激光(高电平关)
	V5_EN = 0;
}

//========================================================================
// 函数: ShutDown
// 描述: 关机休眠.串口通知后IO高阻, P32唤醒后软复位开机.
// 参数: None.
// 返回: None.
//========================================================================
void ShutDown(void)
{
	Output_AllOff();
	Output_Apply();

	printf("Shutdown\r\n");
	while(COM1.busy);                   //等待串口发送完成

	ET0 = 0;
	ES = 0;
	EX0 = 0;
	EA = 0;
	PWM_AllOff();

	P1M0 = 0x00; P1M1 = 0xff;          //全部高阻输入
	P0M0 = 0x00; P0M1 = 0xff;
	P2M0 = 0x00; P2M1 = 0xff;
	P3M0 = 0x00; P3M1 = 0xff;
	P4M0 = 0x00; P4M1 = 0xff;
	P5M0 = 0x00; P5M1 = 0xff;

	P3M1 &= ~0x04;                      //P32保持准双向, 避免高阻浮空误触发INT0
	IT0 = 0;                            //INT0上升沿+下降沿中断
	IE0 = 0;                            //清除挂起标志, 防止进休眠立即唤醒
	EX0 = 1;
	EA = 1;

	PCON = 0x02;                        //MCU进入掉电模式
	_nop_();                            //唤醒后先执行此处,再进INT0中断
	_nop_();
	_nop_();
	_nop_();

	/* 未触发软复位时的兜底恢复 */
	Hw_IO_Init();
	Timer0_Init();
	ES = 1;
	Sys.MidHold_ms = 0;
}
