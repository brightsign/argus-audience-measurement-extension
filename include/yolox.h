#ifndef _RKNN_DEMO_YOLOX_H_
#define _RKNN_DEMO_YOLOX_H_

#include "rknn_api.h"
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOX_THRESH 0.25   // Default box confidence threshold
#define NMS_THRESH 0.45   // Default NMS threshold
#define OBJ_CLASS_NUM 80
#define OBJ_NUMB_MAX_SIZE 128
#define OBJ_NAME_MAX_SIZE 64

// YOLOX model - standard YOLO with DFL encoding and separate box/score tensors

typedef struct {
    rknn_context rknn_ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr *input_attrs;
    rknn_tensor_attr *output_attrs;
    int model_channel;
    int model_width;
    int model_height;
    bool is_quant;
    // YOLOX model type - always standard format
    // Preallocated buffers to avoid malloc/free per frame
    image_buffer_t prealloc_img;
    unsigned char *prealloc_img_data;  // backing buffer for prealloc_img
} rknn_app_context_t;

typedef struct box_rect_t {
    int left;    ///< Most left coordinate
    int top;     ///< Most top coordinate
    int right;   ///< Most right coordinate
    int bottom;  ///< Most bottom coordinate
} box_rect_t;

typedef struct object_detect_result {
    box_rect_t box;
    float prop;
    int cls_id;
    char name[OBJ_NAME_MAX_SIZE];
} object_detect_result_t;

typedef struct object_detect_result_list {
    int count;
    object_detect_result_t results[OBJ_NUMB_MAX_SIZE];
} object_detect_result_list;

int init_yolox_model(const char *model_path, rknn_app_context_t *app_ctx, int npu_core);
int release_yolox_model(rknn_app_context_t *app_ctx);
int inference_yolox_model(rknn_app_context_t *app_ctx, image_buffer_t *img, object_detect_result_list *od_results, float conf_threshold = BOX_THRESH);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif //_RKNN_DEMO_YOLOX_H_