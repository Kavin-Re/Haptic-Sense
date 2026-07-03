/**
******************************************************************************
* @file    app.c
* @author  GPM Application Team
*
******************************************************************************
* @attention
*
* Copyright (c) 2024 STMicroelectronics.
* All rights reserved.
*
* This software is licensed under terms that can be found in the LICENSE file
* in the root directory of this software component.
* If no LICENSE file comes with this software, it is provided AS-IS.
*
******************************************************************************
*/

#include "app.h"

#include <stdint.h>

#include "app_cam.h"
#include "app_config.h"
#include "app_postprocess.h"
#include "isp_api.h"
#include "cmw_camera.h"
#include "scrl.h"
#include "serial_protocol.h"
#include "stm32_lcd.h"
#include "stm32_lcd_ex.h"
#include "stm32n6xx_hal.h"
#include "stm32n6570_discovery.h"
#include "tk/tkernel.h"
#include "tm/tmonitor.h"
#ifdef TRACKER_MODULE
#include "tracker.h"
#endif
#include "network.h"
#include "utils.h"

#define UTKERNEL_STACK_SIZE 2048

#define CACHE_OP(__op__)   \
  do                       \
  {                        \
    if (is_cache_enable()) \
    {                      \
      __op__;              \
    }                      \
  } while (0)

#define ALIGN_VALUE(_v_, _a_) (((_v_) + (_a_) - 1) & ~((_a_) - 1))

#define NN_OUT_NB LL_ATON_DEFAULT_OUT_NUM

#define NN_OUT_MAX_NB 4
#if NN_OUT_NB > NN_OUT_MAX_NB
#error "max output buffer reached"
#endif

#define NN_OUT0_SIZE LL_ATON_DEFAULT_OUT_1_SIZE_BYTES
#define NN_OUT0_SIZE_ALIGN ALIGN_VALUE(NN_OUT0_SIZE, LL_ATON_DEFAULT_OUT_1_ALIGNMENT)
#ifdef LL_ATON_DEFAULT_OUT_2_SIZE_BYTES
#define NN_OUT1_SIZE LL_ATON_DEFAULT_OUT_2_SIZE_BYTES
#define NN_OUT1_SIZE_ALIGN ALIGN_VALUE(NN_OUT1_SIZE, LL_ATON_DEFAULT_OUT_2_ALIGNMENT)
#else
#define NN_OUT1_SIZE 0
#define NN_OUT1_SIZE_ALIGN 0
#endif
#ifdef LL_ATON_DEFAULT_OUT_3_SIZE_BYTES
#define NN_OUT2_SIZE LL_ATON_DEFAULT_OUT_3_SIZE_BYTES
#define NN_OUT2_SIZE_ALIGN ALIGN_VALUE(NN_OUT2_SIZE, LL_ATON_DEFAULT_OUT_3_ALIGNMENT)
#else
#define NN_OUT2_SIZE 0
#define NN_OUT2_SIZE_ALIGN 0
#endif
#ifdef LL_ATON_DEFAULT_OUT_4_SIZE_BYTES
#define NN_OUT3_SIZE LL_ATON_DEFAULT_OUT_4_SIZE_BYTES
#define NN_OUT3_SIZE_ALIGN ALIGN_VALUE(NN_OUT3_SIZE, LL_ATON_DEFAULT_OUT_4_ALIGNMENT)
#else
#define NN_OUT3_SIZE 0
#define NN_OUT3_SIZE_ALIGN 0
#endif

#define NN_OUT_BUFFER_SIZE (NN_OUT0_SIZE_ALIGN + NN_OUT1_SIZE_ALIGN + NN_OUT2_SIZE_ALIGN + NN_OUT3_SIZE_ALIGN)

#define LCD_FG_WIDTH LCD_BG_WIDTH
#define LCD_FG_HEIGHT LCD_BG_HEIGHT

#define NUMBER_COLORS 10
#define BQUEUE_MAX_BUFFERS 2
#define CPU_LOAD_HISTORY_DEPTH 8

#define DISPLAY_BUFFER_NB (DISPLAY_DELAY + 2)

/* Align so we are sure nn_output_buffers[0] and nn_output_buffers[1] are aligned on 32 bytes */
#define NN_BUFFER_OUT_SIZE_ALIGN ALIGN_VALUE(NN_BUFFER_OUT_SIZE, 32)

#define UTIL_LCD_COLOR_TRANSPARENT 0

#ifdef STM32N6570_DK_REV
#define LCD_FONT Font20
#define BUTTON_TOGGLE_TRACKING BUTTON_USER1
#else
#define LCD_FONT Font12
#define BUTTON_TOGGLE_TRACKING BUTTON_USER
#endif

#ifdef TRACKER_MODULE
typedef struct
{
  double cx;
  double cy;
  double w;
  double h;
  uint32_t id;
} tbox_info;
#endif

typedef struct
{
  uint32_t X0;
  uint32_t Y0;
  uint32_t XSize;
  uint32_t YSize;
} Rectangle_TypeDef;

typedef struct
{
  ID free;
  ID ready;
  int buffer_nb;
  uint8_t *buffers[BQUEUE_MAX_BUFFERS];
  int free_idx;
  int ready_idx;
} bqueue_t;

typedef struct
{
  uint64_t current_total;
  uint64_t current_thread_total;
  uint64_t prev_total;
  uint64_t prev_thread_total;
  struct
  {
    uint64_t total;
    uint64_t thread;
    uint32_t tick;
  } history[CPU_LOAD_HISTORY_DEPTH];
} cpuload_info_t;

typedef struct
{
  int32_t nb_detect;
  od_pp_outBuffer_t detects[AI_OD_PP_MAX_BOXES_LIMIT];
  int tracking_enabled;
#ifdef TRACKER_MODULE
  int tboxes_valid_nb;
  tbox_info tboxes[AI_OD_PP_MAX_BOXES_LIMIT];
#endif
  uint32_t nn_period_ms;
  uint32_t inf_ms;
  uint32_t pp_ms;
  uint32_t disp_ms;
} display_info_t;

typedef struct
{
  ID update;
  ID lock;
  display_info_t info;
} display_t;

/* Globals */
DECLARE_CLASSES_TABLE;
/* Lcd Background area */
static Rectangle_TypeDef lcd_bg_area = {
    .X0 = 0,
    .Y0 = 0,
    .XSize = LCD_BG_WIDTH,
    .YSize = LCD_BG_HEIGHT,
};
/* Lcd Foreground area */
static Rectangle_TypeDef lcd_fg_area = {
    .X0 = 0,
    .Y0 = 0,
    .XSize = LCD_FG_WIDTH,
    .YSize = LCD_FG_HEIGHT,
};
static const uint32_t colors[NUMBER_COLORS] = {
    UTIL_LCD_COLOR_GREEN,
    UTIL_LCD_COLOR_RED,
    UTIL_LCD_COLOR_CYAN,
    UTIL_LCD_COLOR_MAGENTA,
    UTIL_LCD_COLOR_YELLOW,
    UTIL_LCD_COLOR_GRAY,
    UTIL_LCD_COLOR_BLACK,
    UTIL_LCD_COLOR_BROWN,
    UTIL_LCD_COLOR_BLUE,
    UTIL_LCD_COLOR_ORANGE};
/* Lcd Background Buffer */
static uint8_t lcd_bg_buffer[DISPLAY_BUFFER_NB][LCD_BG_WIDTH * LCD_BG_HEIGHT * 2] ALIGN_32 IN_PSRAM;
static int lcd_bg_buffer_disp_idx = 1;
static int lcd_bg_buffer_capt_idx = 0;
/* Lcd Foreground Buffer */
static uint8_t lcd_fg_buffer[2][LCD_FG_WIDTH * LCD_FG_HEIGHT * 2] ALIGN_32 IN_PSRAM;
static int lcd_fg_buffer_rd_idx;
static display_t disp;
static cpuload_info_t cpu_load;
/* screen buffer */
static uint8_t screen_buffer[LCD_BG_WIDTH * LCD_BG_HEIGHT * 2] ALIGN_32 IN_PSRAM;

/* model */
LL_ATON_DECLARE_NAMED_NN_INSTANCE_AND_INTERFACE(Default);
/* nn input buffers */
static uint8_t nn_input_buffers[2][NN_WIDTH * NN_HEIGHT * NN_BPP] ALIGN_32 IN_PSRAM;
static bqueue_t nn_input_queue;
/* nn output buffers */
static const uint32_t nn_out_len_user[NN_OUT_MAX_NB] = {
    NN_OUT0_SIZE, NN_OUT1_SIZE, NN_OUT2_SIZE, NN_OUT3_SIZE};
static uint8_t nn_output_buffers[2][NN_OUT_BUFFER_SIZE] ALIGN_32;
static bqueue_t nn_output_queue;

/* rtos */
static UB nn_thread_stack[2 * UTKERNEL_STACK_SIZE];
static ID nn_task_id;
static UB pp_thread_stack[2 * UTKERNEL_STACK_SIZE];
static ID pp_task_id;
static UB dp_thread_stack[2 * UTKERNEL_STACK_SIZE];
static ID dp_task_id;
static UB isp_thread_stack[2 * UTKERNEL_STACK_SIZE];
static ID isp_task_id;
static ID isp_sem;

/* tracking state */
#ifdef TRACKER_MODULE
static trk_tbox_t tboxes[2 * AI_OD_PP_MAX_BOXES_LIMIT];
static trk_dbox_t dboxes[AI_OD_PP_MAX_BOXES_LIMIT];
static trk_ctx_t trk_ctx;
#endif

static int is_cache_enable()
{
#if defined(USE_DCACHE)
  return 1;
#else
  return 0;
#endif
}

static void cpuload_init(cpuload_info_t *cpu_load)
{
  memset(cpu_load, 0, sizeof(cpuload_info_t));
}

static void cpuload_update(cpuload_info_t *cpu_load)
{
  int i;

  cpu_load->history[1] = cpu_load->history[0];
  cpu_load->history[0].total = HAL_GetTick();
  cpu_load->history[0].thread = HAL_GetTick();
  cpu_load->history[0].tick = HAL_GetTick();

  if (cpu_load->history[1].tick - cpu_load->history[2].tick < 1000)
    return;

  for (i = 0; i < CPU_LOAD_HISTORY_DEPTH - 2; i++)
    cpu_load->history[CPU_LOAD_HISTORY_DEPTH - 1 - i] = cpu_load->history[CPU_LOAD_HISTORY_DEPTH - 1 - i - 1];
}

static void cpuload_get_info(cpuload_info_t *cpu_load, float *cpu_load_last, float *cpu_load_last_second,
                             float *cpu_load_last_five_seconds)
{
  if (cpu_load_last)
    *cpu_load_last = 100.0 * (cpu_load->history[0].thread - cpu_load->history[1].thread) /
                     (cpu_load->history[0].total - cpu_load->history[1].total);
  if (cpu_load_last_second)
    *cpu_load_last_second = 100.0 * (cpu_load->history[2].thread - cpu_load->history[3].thread) /
                            (cpu_load->history[2].total - cpu_load->history[3].total);
  if (cpu_load_last_five_seconds)
    *cpu_load_last_five_seconds = 100.0 * (cpu_load->history[2].thread - cpu_load->history[7].thread) /
                                  (cpu_load->history[2].total - cpu_load->history[7].total);
}

static int bqueue_init(bqueue_t *bq, int buffer_nb, uint8_t **buffers)
{
  int i;

  if (buffer_nb > BQUEUE_MAX_BUFFERS)
    return -1;

  T_CSEM sem_config;
  sem_config.sematr = TA_TFIFO;
  sem_config.exinf = NULL;

  sem_config.isemcnt = buffer_nb;
  sem_config.maxsem = buffer_nb;
  bq->free = tk_cre_sem(&sem_config);
  if (bq->free <= 0)
    goto free_sem_error;

  sem_config.isemcnt = 0;
  sem_config.maxsem = buffer_nb;
  bq->ready = tk_cre_sem(&sem_config);
  if (bq->ready <= 0)
    goto ready_sem_error;

  bq->buffer_nb = buffer_nb;
  for (i = 0; i < buffer_nb; i++)
  {
    assert(buffers[i]);
    bq->buffers[i] = buffers[i];
  }
  bq->free_idx = 0;
  bq->ready_idx = 0;

  return 0;

ready_sem_error:
  tk_del_sem(bq->free);
free_sem_error:
  return -1;
}

static uint8_t *bqueue_get_free(bqueue_t *bq, int is_blocking)
{
  uint8_t *res;
  ER ret;

  ret = tk_wai_sem(bq->free, 1, is_blocking ? TMO_FEVR : TMO_POL);
  if (ret != E_OK)
    return NULL;

  res = bq->buffers[bq->free_idx];
  bq->free_idx = (bq->free_idx + 1) % bq->buffer_nb;

  return res;
}

static void bqueue_put_free(bqueue_t *bq)
{
  ER ret;

  ret = tk_sig_sem(bq->free, 1);
  assert(ret == E_OK);
}

static uint8_t *bqueue_get_ready(bqueue_t *bq)
{
  uint8_t *res;
  ER ret;

  ret = tk_wai_sem(bq->ready, 1, TMO_FEVR);
  assert(ret == E_OK);

  res = bq->buffers[bq->ready_idx];
  bq->ready_idx = (bq->ready_idx + 1) % bq->buffer_nb;

  return res;
}

static void bqueue_put_ready(bqueue_t *bq)
{
  ER ret;

  ret = tk_sig_sem(bq->ready, 1);
  assert(ret == E_OK);
}

static void reload_bg_layer(int next_disp_idx)
{
  int ret;

  ret = SCRL_SetAddress_NoReload(lcd_bg_buffer[next_disp_idx], SCRL_LAYER_0);
  assert(ret == 0);
  ret = SCRL_ReloadLayer(SCRL_LAYER_0);
  assert(ret == 0);

  ret = SRCL_Update();
  assert(ret == 0);
}

static void app_main_pipe_frame_event()
{
  int next_disp_idx = (lcd_bg_buffer_disp_idx + 1) % DISPLAY_BUFFER_NB;
  int next_capt_idx = (lcd_bg_buffer_capt_idx + 1) % DISPLAY_BUFFER_NB;
  int ret;

  ret = HAL_DCMIPP_PIPE_SetMemoryAddress(CMW_CAMERA_GetDCMIPPHandle(), DCMIPP_PIPE1,
                                         DCMIPP_MEMORY_ADDRESS_0, (uint32_t)lcd_bg_buffer[next_capt_idx]);
  assert(ret == HAL_OK);

  reload_bg_layer(next_disp_idx);
  lcd_bg_buffer_disp_idx = next_disp_idx;
  lcd_bg_buffer_capt_idx = next_capt_idx;
}

static void app_ancillary_pipe_frame_event()
{
  uint8_t *next_buffer;
  int ret;

  next_buffer = bqueue_get_free(&nn_input_queue, 0);
  if (next_buffer)
  {
    ret = HAL_DCMIPP_PIPE_SetMemoryAddress(CMW_CAMERA_GetDCMIPPHandle(), DCMIPP_PIPE2,
                                           DCMIPP_MEMORY_ADDRESS_0, (uint32_t)next_buffer);
    assert(ret == HAL_OK);
    bqueue_put_ready(&nn_input_queue);
  }
}

static void app_main_pipe_vsync_event()
{
  ER ret;

  ret = tk_sig_sem(isp_sem, 1);
  assert(ret == E_OK);
}

static int clamp_point(int *x, int *y)
{
  int xi = *x;
  int yi = *y;

  if (*x < 0)
    *x = 0;
  if (*y < 0)
    *y = 0;
  if (*x >= lcd_bg_area.XSize)
    *x = lcd_bg_area.XSize - 1;
  if (*y >= lcd_bg_area.YSize)
    *y = lcd_bg_area.YSize - 1;

  return (xi != *x) || (yi != *y);
}

static void convert_length(float32_t wi, float32_t hi, int *wo, int *ho)
{
  *wo = (int)(lcd_bg_area.XSize * wi);
  *ho = (int)(lcd_bg_area.YSize * hi);
}

static void convert_point(float32_t xi, float32_t yi, int *xo, int *yo)
{
  *xo = (int)(lcd_bg_area.XSize * xi);
  *yo = (int)(lcd_bg_area.YSize * yi);
}

static void Display_Detection(od_pp_outBuffer_t *detect)
{
  int xc, yc;
  int x0, y0;
  int x1, y1;
  int w, h;

  convert_point(detect->x_center, detect->y_center, &xc, &yc);
  convert_length(detect->width, detect->height, &w, &h);
  x0 = xc - (w + 1) / 2;
  y0 = yc - (h + 1) / 2;
  x1 = xc + (w + 1) / 2;
  y1 = yc + (h + 1) / 2;
  clamp_point(&x0, &y0);
  clamp_point(&x1, &y1);

  UTIL_LCD_DrawRect(x0, y0, x1 - x0, y1 - y0, colors[detect->class_index % NUMBER_COLORS]);
  UTIL_LCDEx_PrintfAt(x0 + 1, y0 + 1, LEFT_MODE, classes_table[detect->class_index]);
}

static void Display_NetworkOutput_NoTracking(display_info_t *info)
{
  od_pp_outBuffer_t *rois = info->detects;
  uint32_t nb_rois = info->nb_detect;
  float cpu_load_one_second;
  int line_nb = 0;
  float nn_fps;
  int i;
  static uint32_t frame_count = 0;

  /* Output structured detection data */
  SERIAL_DETECTION_START(frame_count, nb_rois, 0);
  for (i = 0; i < nb_rois; i++)
  {
    int xc, yc, w, h, x0, y0;
    convert_point(rois[i].x_center, rois[i].y_center, &xc, &yc);
    convert_length(rois[i].width, rois[i].height, &w, &h);
    x0 = xc - (w + 1) / 2;
    y0 = yc - (h + 1) / 2;
    clamp_point(&x0, &y0);
    SERIAL_DETECTION_OBJECT(i == 0, classes_table[rois[i].class_index],
                           x0, y0, w, h);
  }
  SERIAL_DETECTION_END();
  frame_count++;

  /* clear previous ui */
  UTIL_LCD_FillRect(lcd_fg_area.X0, lcd_fg_area.Y0, lcd_fg_area.XSize, lcd_fg_area.YSize, 0x00000000);

  /* cpu load */
  cpuload_update(&cpu_load);
  cpuload_get_info(&cpu_load, NULL, &cpu_load_one_second, NULL);

  /* draw metrics */
  nn_fps = 1000.0 / info->nn_period_ms;
#if 1
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "Inference");
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "   %ums", info->inf_ms);
  line_nb += 2;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, " Persons %u", nb_rois);
  line_nb += 1;
#else
  (void)nn_fps;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "Cpu load");
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "   %.1f%%", cpu_load_one_second);
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "nn period");
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "   %ums", info->nn_period_ms);
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "Inference");
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "   %ums", info->inf_ms);
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "Post process");
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "   %ums", info->pp_ms);
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "Display");
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "   %ums", info->disp_ms);
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, " Persons %u", nb_rois);
  line_nb += 1;
#endif

  /* Draw bounding boxes */
  for (i = 0; i < nb_rois; i++)
    Display_Detection(&rois[i]);
}

static int model_get_output_nb(const LL_Buffer_InfoTypeDef *nn_out_info)
{
  int nb = 0;

  while (nn_out_info->name)
  {
    nb++;
    nn_out_info++;
  }

  return nb;
}

#ifdef TRACKER_MODULE
static void Display_TrackingBox(tbox_info *tbox)
{
  int xc, yc;
  int x0, y0;
  int x1, y1;
  int w, h;

  convert_point(tbox->cx, tbox->cy, &xc, &yc);
  convert_length(tbox->w, tbox->h, &w, &h);
  x0 = xc - (w + 1) / 2;
  y0 = yc - (h + 1) / 2;
  x1 = xc + (w + 1) / 2;
  y1 = yc + (h + 1) / 2;
  clamp_point(&x0, &y0);
  clamp_point(&x1, &y1);

  UTIL_LCD_DrawRect(x0, y0, x1 - x0, y1 - y0, colors[tbox->id % NUMBER_COLORS]);
  UTIL_LCDEx_PrintfAt(x0 + 1, y0 + 1, LEFT_MODE, "%3d", tbox->id);
}

static void Display_NetworkOutput_Tracking(display_info_t *info)
{
  float cpu_load_one_second;
  int line_nb = 0;
  float nn_fps;
  int i;
  static uint32_t frame_count = 0;

  /* Output structured tracking data */
  SERIAL_DETECTION_START(frame_count, info->tboxes_valid_nb, 1);
  for (i = 0; i < info->tboxes_valid_nb; i++)
  {
    int xc, yc, w, h, x0, y0;
    convert_point(info->tboxes[i].cx, info->tboxes[i].cy, &xc, &yc);
    convert_length(info->tboxes[i].w, info->tboxes[i].h, &w, &h);
    x0 = xc - (w + 1) / 2;
    y0 = yc - (h + 1) / 2;
    clamp_point(&x0, &y0);
    SERIAL_TRACKING_OBJECT(i == 0, info->tboxes[i].id, "person",
                          x0, y0, w, h);
  }
  SERIAL_DETECTION_END();
  frame_count++;

  /* clear previous ui */
  UTIL_LCD_FillRect(lcd_fg_area.X0, lcd_fg_area.Y0, lcd_fg_area.XSize, lcd_fg_area.YSize, 0x00000000);

  /* cpu load */
  cpuload_update(&cpu_load);
  cpuload_get_info(&cpu_load, NULL, &cpu_load_one_second, NULL);

  /* draw metrics */
  nn_fps = 1000.0 / info->nn_period_ms;
#if 1
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "Inference");
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "   %ums", info->inf_ms);
  line_nb += 2;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, " Persons %u", info->tboxes_valid_nb);
  line_nb += 1;
#else
  (void)nn_fps;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "Cpu load");
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "   %.1f%%", cpu_load_one_second);
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "nn period");
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "   %ums", info->nn_period_ms);
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "Inference");
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "   %ums", info->inf_ms);
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "Post process");
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "   %ums", info->pp_ms);
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "Display");
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, "   %ums", info->disp_ms);
  line_nb += 1;
  UTIL_LCDEx_PrintfAt(0, LINE(line_nb), RIGHT_MODE, " Persons %u", info->tboxes_valid_nb);
  line_nb += 1;
#endif

  /* Draw bounding boxes */
  for (i = 0; i < info->tboxes_valid_nb; i++)
  {
    Display_TrackingBox(&info->tboxes[i]);
  }
}
#else
static void Display_NetworkOutput_Tracking(display_info_t *info)
{
  /* You should not be here */
  assert(0);
}
#endif

static void Display_NetworkOutput(display_info_t *info)
{
  if (info->tracking_enabled)
    Display_NetworkOutput_Tracking(info);
  else
    Display_NetworkOutput_NoTracking(info);
}

static void nn_thread_fct(INT stacd, void *exinf)
{
  SERIAL_MSG_STR(LEVEL_INFO, MODULE_NN, EVENT_TASK_START, "thread", "nn_inference");
  const LL_Buffer_InfoTypeDef *nn_out_info = LL_ATON_Output_Buffers_Info(&NN_Instance_Default);
  const LL_Buffer_InfoTypeDef *nn_in_info = LL_ATON_Input_Buffers_Info(&NN_Instance_Default);
  uint32_t nn_period_ms;
  uint32_t nn_period[2];
  uint8_t *nn_pipe_dst;
  uint32_t nn_in_len;
  uint32_t inf_ms;
  uint32_t ts;
  int ret;
  int i;

  /* Initialize Cube.AI/ATON ... */
  LL_ATON_RT_RuntimeInit();
  /* ... and model instance */
  LL_ATON_RT_Init_Network(&NN_Instance_Default);

  /* setup buffers size */
  nn_in_len = LL_Buffer_len(&nn_in_info[0]);
  assert(NN_OUT_NB == model_get_output_nb(nn_out_info));
  for (i = 0; i < NN_OUT_NB; i++)
    assert(LL_Buffer_len(&nn_out_info[i]) == nn_out_len_user[i]);

  /*** App Loop ***************************************************************/
  nn_period[1] = HAL_GetTick();

  nn_pipe_dst = bqueue_get_free(&nn_input_queue, 0);
  assert(nn_pipe_dst);
  CAM_NNPipe_Start(nn_pipe_dst, CMW_MODE_CONTINUOUS);
  SERIAL_MSG_STR(LEVEL_INFO, MODULE_NN, EVENT_TASK_READY, "thread", "nn_inference");
  while (1)
  {
    uint8_t *capture_buffer;
    uint8_t *out[NN_OUT_NB];
    uint8_t *output_buffer;
    int i;

    nn_period[0] = nn_period[1];
    nn_period[1] = HAL_GetTick();
    nn_period_ms = nn_period[1] - nn_period[0];

    capture_buffer = bqueue_get_ready(&nn_input_queue);
    assert(capture_buffer);
    output_buffer = bqueue_get_free(&nn_output_queue, 1);
    assert(output_buffer);
    out[0] = output_buffer;
    for (i = 1; i < NN_OUT_NB; i++)
      out[i] = out[i - 1] + ALIGN_VALUE(nn_out_len_user[i - 1], 32);

    /* run ATON inference */
    ts = HAL_GetTick();
    /* Note that we don't need to clean/invalidate those input buffers since they are only access in hardware */
    ret = LL_ATON_Set_User_Input_Buffer_Default(0, capture_buffer, nn_in_len);
    assert(ret == LL_ATON_User_IO_NOERROR);
    /* Invalidate output buffer before Hw access it */
    CACHE_OP(SCB_InvalidateDCache_by_Addr(output_buffer, sizeof(nn_output_buffers[0])));
    for (i = 0; i < NN_OUT_NB; i++)
    {
      ret = LL_ATON_Set_User_Output_Buffer_Default(i, out[i], nn_out_len_user[i]);
      assert(ret == LL_ATON_User_IO_NOERROR);
    }
    Run_Inference(&NN_Instance_Default);
    inf_ms = HAL_GetTick() - ts;

    /* release buffers */
    bqueue_put_free(&nn_input_queue);
    bqueue_put_ready(&nn_output_queue);

    /* update display stats */
    ret = tk_loc_mtx(disp.lock, TMO_FEVR);
    assert(ret == E_OK);
    disp.info.inf_ms = inf_ms;
    disp.info.nn_period_ms = nn_period_ms;
    ret = tk_unl_mtx(disp.lock);
    assert(ret == E_OK);
  }
  tk_ext_tsk();
}

#ifdef TRACKER_MODULE
static int TRK_Init()
{
  const trk_conf_t cfg = {
      .track_thresh = 0.25,
      .det_thresh = 0.8,
      .sim1_thresh = 0.8,
      .sim2_thresh = 0.5,
      .tlost_cnt = 30,
  };

  return trk_init(&trk_ctx, (trk_conf_t *)&cfg, ARRAY_NB(tboxes), tboxes);
}

static int update_and_capture_tracking_enabled()
{
  static int prev_button_state = GPIO_PIN_RESET;
  static int tracking_enabled = 1;
  int cur_button_state;
  int ret;

  cur_button_state = BSP_PB_GetState(BUTTON_TOGGLE_TRACKING);
  if (cur_button_state == GPIO_PIN_SET && prev_button_state == GPIO_PIN_RESET)
  {
    tracking_enabled = !tracking_enabled;
    if (tracking_enabled)
    {
      printf("Enable tracking\n");
      ret = TRK_Init();
      assert(ret == 0);
    }
    else
      printf("Disable tracking\n");
  }
  prev_button_state = cur_button_state;

  return tracking_enabled;
}

static void roi_to_dbox(od_pp_outBuffer_t *roi, trk_dbox_t *dbox)
{
  dbox->conf = roi->conf;
  dbox->cx = roi->x_center;
  dbox->cy = roi->y_center;
  dbox->w = roi->width;
  dbox->h = roi->height;
}

static int app_tracking(od_pp_out_t *pp)
{
  int tracking_enabled = update_and_capture_tracking_enabled();
  int ret;
  int i;

  if (!tracking_enabled)
    return 0;

  for (i = 0; i < pp->nb_detect; i++)
    roi_to_dbox(&pp->pOutBuff[i], &dboxes[i]);

  ret = trk_update(&trk_ctx, pp->nb_detect, dboxes);
  assert(ret == 0);

  return 1;
}

static void tbox_to_tbox_info(trk_tbox_t *tbox, tbox_info *tinfo)
{
  tinfo->cx = tbox->cx;
  tinfo->cy = tbox->cy;
  tinfo->w = tbox->w;
  tinfo->h = tbox->h;
  tinfo->id = tbox->id;
}
#else
static int app_tracking(od_pp_out_t *pp)
{
  return 0;
}
#endif

static void pp_thread_fct(INT stacd, void *exinf)
{
  SERIAL_MSG_STR(LEVEL_INFO, MODULE_PP, EVENT_TASK_START, "thread", "postprocess");
#if POSTPROCESS_TYPE == POSTPROCESS_OD_YOLO_V2_UF
  od_yolov2_pp_static_param_t pp_params;
#elif POSTPROCESS_TYPE == POSTPROCESS_OD_YOLO_V5_UU
  od_yolov5_pp_static_param_t pp_params;
#elif POSTPROCESS_TYPE == POSTPROCESS_OD_YOLO_V8_UF || POSTPROCESS_TYPE == POSTPROCESS_OD_YOLO_V8_UI
  od_yolov8_pp_static_param_t pp_params;
#elif POSTPROCESS_TYPE == POSTPROCESS_OD_ST_YOLOX_UF
  od_st_yolox_pp_static_param_t pp_params;
#else
#error "PostProcessing type not supported"
#endif
  uint8_t *pp_input[NN_OUT_NB];
  od_pp_out_t pp_output;
  int tracking_enabled;
  uint32_t nn_pp[2];
  int ret;
  int i;

  (void)tracking_enabled;
  /* setup post process */
  app_postprocess_init(&pp_params, &NN_Instance_Default);
  while (1)
  {
    uint8_t *output_buffer;

    output_buffer = bqueue_get_ready(&nn_output_queue);
    assert(output_buffer);
    pp_input[0] = output_buffer;
    for (i = 1; i < NN_OUT_NB; i++)
      pp_input[i] = pp_input[i - 1] + ALIGN_VALUE(nn_out_len_user[i - 1], 32);
    pp_output.pOutBuff = NULL;

    nn_pp[0] = HAL_GetTick();
    ret = app_postprocess_run((void **)pp_input, NN_OUT_NB, &pp_output, &pp_params);
    assert(ret == 0);
    tracking_enabled = app_tracking(&pp_output);

    nn_pp[1] = HAL_GetTick();

    /* update display stats and detection info */
    ret = tk_loc_mtx(disp.lock, TMO_FEVR);
    assert(ret == E_OK);

    /* Clamp nb_detect to prevent buffer overflow */
    if (pp_output.nb_detect > AI_OD_PP_MAX_BOXES_LIMIT)
    {
      SERIAL_MSG_HEADER(LEVEL_WARN, MODULE_PP, EVENT_WARNING);
      tm_printf((UB *)"{\"msg\":\"detection_overflow\",\"detected\":%u,\"limit\":%d}\n",
                pp_output.nb_detect, AI_OD_PP_MAX_BOXES_LIMIT);
      disp.info.nb_detect = AI_OD_PP_MAX_BOXES_LIMIT;
    }
    else
    {
      disp.info.nb_detect = pp_output.nb_detect;
    }
    for (i = 0; i < disp.info.nb_detect; i++)
    {
      disp.info.detects[i] = pp_output.pOutBuff[i];
    }
#ifdef TRACKER_MODULE
    disp.info.tracking_enabled = tracking_enabled;
    disp.info.tboxes_valid_nb = 0;
    for (i = 0; i < ARRAY_NB(tboxes); i++)
    {
      if (!tboxes[i].is_tracking || tboxes[i].tlost_cnt)
        continue;
      tbox_to_tbox_info(&tboxes[i], &disp.info.tboxes[disp.info.tboxes_valid_nb]);
      disp.info.tboxes_valid_nb++;
    }
#endif
    disp.info.pp_ms = nn_pp[1] - nn_pp[0];
    ret = tk_unl_mtx(disp.lock);
    assert(ret == E_OK);

    bqueue_put_free(&nn_output_queue);
    /* It's possible xqueue is empty if display is slow. So don't check error code that may by E_QOVR in that case */
    tk_sig_sem(disp.update, 1);
  }
  tk_ext_tsk();
}

static void dp_update_drawing_area()
{
  int ret;
  int write_idx;

  /* Toggle to the write buffer (the one NOT currently being displayed) */
  write_idx = 1 - lcd_fg_buffer_rd_idx;

  __disable_irq();
  ret = SCRL_SetAddress_NoReload(lcd_fg_buffer[write_idx], SCRL_LAYER_1);
  assert(ret == HAL_OK);
  __enable_irq();
}

static void dp_commit_drawing_area()
{
  int ret;

  __disable_irq();
  ret = SCRL_ReloadLayer(SCRL_LAYER_1);
  assert(ret == HAL_OK);
  __enable_irq();

  /* Update the read index to point to the buffer we just committed */
  lcd_fg_buffer_rd_idx = 1 - lcd_fg_buffer_rd_idx;
}

static void dp_thread_fct(INT stacd, void *exinf)
{
  SERIAL_MSG_STR(LEVEL_INFO, MODULE_DISPLAY, EVENT_TASK_START, "thread", "display");
  uint32_t disp_ms = 0;
  display_info_t info;
  uint32_t ts;
  ER ret;
  static uint32_t frame_count = 0;

  while (1)
  {
    ret = tk_wai_sem(disp.update, 1, TMO_FEVR);
    assert(ret == E_OK);

    ret = tk_loc_mtx(disp.lock, TMO_FEVR);
    assert(ret == E_OK);
    info = disp.info;
    ret = tk_unl_mtx(disp.lock);
    assert(ret == E_OK);
    info.disp_ms = disp_ms;

    ts = HAL_GetTick();
    SERIAL_FRAME_EVENT(frame_count, info.nb_detect, info.tracking_enabled, lcd_fg_buffer_rd_idx);

    /* Set up the write buffer for drawing (the inactive buffer) */
    dp_update_drawing_area();
    UTIL_LCD_SetLayer(SCRL_LAYER_1);

    /* Draw to the write buffer */
    Display_NetworkOutput(&info);

    /* Clean cache for the write buffer (the one we just drew to) */
    int write_idx = 1 - lcd_fg_buffer_rd_idx;
    SCB_CleanDCache_by_Addr(lcd_fg_buffer[write_idx], LCD_FG_WIDTH * LCD_FG_HEIGHT * 2);

    /* Commit and swap buffers */
    dp_commit_drawing_area();
    disp_ms = HAL_GetTick() - ts;

    /* Output performance timing */
    SERIAL_PERF_TIMING(frame_count, info.inf_ms, info.pp_ms, disp_ms, info.nn_period_ms);
    frame_count++;
  }
  tk_ext_tsk();
}

static void isp_thread_fct(INT stacd, void *exinf)
{
  SERIAL_MSG_STR(LEVEL_INFO, MODULE_CAMERA, EVENT_TASK_START, "thread", "isp");
  ER ret;

  while (1)
  {
    ret = tk_wai_sem(isp_sem, 1, TMO_FEVR);
    assert(ret == E_OK);

    CAM_IspUpdate();
  }
  tk_ext_tsk();
}

static void Display_init()
{
  SCRL_LayerConfig layers_config[2] = {
      {
          .origin = {lcd_bg_area.X0, lcd_bg_area.Y0},
          .size = {lcd_bg_area.XSize, lcd_bg_area.YSize},
          .format = SCRL_RGB565,
          .address = lcd_bg_buffer[lcd_bg_buffer_disp_idx],
      },
      {
          .origin = {lcd_fg_area.X0, lcd_fg_area.Y0},
          .size = {lcd_fg_area.XSize, lcd_fg_area.YSize},
          .format = SCRL_ARGB4444,
          .address = lcd_fg_buffer[1],
      },
  };
  SCRL_ScreenConfig screen_config = {
      .size = {lcd_bg_area.XSize, lcd_bg_area.YSize},
#ifdef SCR_LIB_USE_SPI
      .format = SCRL_RGB565,
#else
      .format = SCRL_YUV422, /* Use SCRL_RGB565 if host support this format to reduce cpu load */
#endif
      .address = screen_buffer,
      .fps = CAMERA_FPS,
  };
  int ret;

  ret = SCRL_Init((SCRL_LayerConfig *[2]){&layers_config[0], &layers_config[1]}, &screen_config);
  assert(ret == 0);

  UTIL_LCD_SetLayer(SCRL_LAYER_1);
  UTIL_LCD_Clear(UTIL_LCD_COLOR_TRANSPARENT);
  UTIL_LCD_SetFont(&LCD_FONT);
  UTIL_LCD_SetTextColor(UTIL_LCD_COLOR_WHITE);
}

void app_run()
{
  PRI isp_priority = 10;
  PRI nn_priority = 11;
  PRI pp_priority = 14;
  PRI dp_priority = 14;
  ER ret;

  SERIAL_MSG(LEVEL_INFO, MODULE_INIT, "APP_INIT_START");
  /* Enable DWT so DWT_CYCCNT works when debugger not attached */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

  /* screen init */
  memset(lcd_bg_buffer, 0, sizeof(lcd_bg_buffer));
  CACHE_OP(SCB_CleanInvalidateDCache_by_Addr(lcd_bg_buffer, sizeof(lcd_bg_buffer)));
  memset(lcd_fg_buffer, 0, sizeof(lcd_fg_buffer));
  CACHE_OP(SCB_CleanInvalidateDCache_by_Addr(lcd_fg_buffer, sizeof(lcd_fg_buffer)));
  Display_init();

  /* create buffer queues */
  ret = bqueue_init(&nn_input_queue, 2, (uint8_t *[2]){nn_input_buffers[0], nn_input_buffers[1]});
  assert(ret == 0);
  ret = bqueue_init(&nn_output_queue, 2, (uint8_t *[2]){nn_output_buffers[0], nn_output_buffers[1]});
  assert(ret == 0);

#ifdef TRACKER_MODULE
  ret = TRK_Init();
  assert(ret == 0);
  ret = BSP_PB_Init(BUTTON_TOGGLE_TRACKING, BUTTON_MODE_GPIO);
  assert(ret == BSP_ERROR_NONE);
#endif

  cpuload_init(&cpu_load);

  /*** Camera Init ************************************************************/
  SERIAL_MSG(LEVEL_INFO, MODULE_CAMERA, EVENT_CAMERA_INIT);
  CAM_Init();

  /* sems + mutex init */
  T_CSEM sem_config;
  sem_config.sematr = TA_TFIFO;
  sem_config.isemcnt = 0;
  sem_config.maxsem = 1;
  sem_config.exinf = NULL;

  isp_sem = tk_cre_sem(&sem_config);
  assert(isp_sem > 0);

  disp.update = tk_cre_sem(&sem_config);
  assert(disp.update > 0);

  T_CMTX mtx_config;
  mtx_config.mtxatr = TA_TFIFO;
  mtx_config.ceilpri = 0;
  mtx_config.exinf = NULL;

  disp.lock = tk_cre_mtx(&mtx_config);
  assert(disp.lock > 0);

  /* Start LCD Display camera pipe stream */
  CAM_DisplayPipe_Start(lcd_bg_buffer[0], CMW_MODE_CONTINUOUS);

  /* threads init */
  T_CTSK task_config;
  task_config.tskatr = TA_HLNG | TA_RNG3 | TA_USERBUF;
  task_config.stksz = 2 * UTKERNEL_STACK_SIZE;
  task_config.exinf = NULL;

  task_config.task = nn_thread_fct;
  task_config.itskpri = nn_priority;
  task_config.bufptr = nn_thread_stack;
  nn_task_id = tk_cre_tsk(&task_config);
  SERIAL_MSG_TASK(LEVEL_INFO, MODULE_RTOS, EVENT_TASK_CREATE, "nn_thread", nn_task_id);
  tk_sta_tsk(nn_task_id, 0);

  task_config.task = pp_thread_fct;
  task_config.itskpri = pp_priority;
  task_config.bufptr = pp_thread_stack;
  pp_task_id = tk_cre_tsk(&task_config);
  SERIAL_MSG_TASK(LEVEL_INFO, MODULE_RTOS, EVENT_TASK_CREATE, "pp_thread", pp_task_id);
  tk_sta_tsk(pp_task_id, 0);

  task_config.task = dp_thread_fct;
  task_config.itskpri = dp_priority;
  task_config.bufptr = dp_thread_stack;
  dp_task_id = tk_cre_tsk(&task_config);
  SERIAL_MSG_TASK(LEVEL_INFO, MODULE_RTOS, EVENT_TASK_CREATE, "dp_thread", dp_task_id);
  tk_sta_tsk(dp_task_id, 0);

  task_config.task = isp_thread_fct;
  task_config.itskpri = isp_priority;
  task_config.bufptr = isp_thread_stack;
  isp_task_id = tk_cre_tsk(&task_config);
  SERIAL_MSG_TASK(LEVEL_INFO, MODULE_RTOS, EVENT_TASK_CREATE, "isp_thread", isp_task_id);
  tk_sta_tsk(isp_task_id, 0);

  SERIAL_MSG(LEVEL_INFO, MODULE_INIT, "APP_INIT_COMPLETE");
}

int CMW_CAMERA_PIPE_FrameEventCallback(uint32_t pipe)
{
  if (pipe == DCMIPP_PIPE1)
    app_main_pipe_frame_event();
  else if (pipe == DCMIPP_PIPE2)
    app_ancillary_pipe_frame_event();

  return HAL_OK;
}

int CMW_CAMERA_PIPE_VsyncEventCallback(uint32_t pipe)
{
  if (pipe == DCMIPP_PIPE1)
    app_main_pipe_vsync_event();

  return HAL_OK;
}
