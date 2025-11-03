#ifndef _POSTPROCESS_H_
#define _POSTPROCESS_H_

#include "rknn_api.h"
#include "common.h"
#include "yolox.h"
#include "image_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

int post_process(rknn_app_context_t* app_ctx, void* outputs, letterbox_t* letter_box, 
                 float conf_threshold, float nms_threshold, object_detect_result_list* od_results);
int init_post_process();
void deinit_post_process();
char *coco_cls_to_name(int cls_id);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // _POSTPROCESS_H_