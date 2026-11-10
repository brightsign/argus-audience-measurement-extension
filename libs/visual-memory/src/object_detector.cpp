// SPDX-License-Identifier: MIT
#include "visualmemory/object_detector.h"

namespace visualmemory {

const std::vector<std::string>& coco80_names() {
    static const std::vector<std::string> n = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck",
    "boat", "traffic_light", "fire_hydrant", "stop_sign", "parking_meter", "bench", "bird", "cat",
    "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe",
    "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard",
    "sports_ball", "kite", "baseball_bat", "baseball_glove", "skateboard", "surfboard", "tennis_racket", "bottle",
    "wine_glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot_dog", "pizza", "donut", "cake",
    "chair", "couch", "potted_plant", "bed", "dining_table", "toilet", "tv", "laptop",
    "mouse", "remote", "keyboard", "cell_phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy_bear", "hair_drier", "toothbrush"};
    return n;
}

int coco80_id(const std::string& name) {
    const auto& n = coco80_names();
    for (size_t i = 0; i < n.size(); ++i) if (n[i] == name) return static_cast<int>(i);
    std::string alt = name;
    for (char& c : alt) if (c == ' ') c = '_';
    for (size_t i = 0; i < n.size(); ++i) if (n[i] == alt) return static_cast<int>(i);
    return -1;
}

const char* coco80_name(int id) {
    const auto& n = coco80_names();
    return (id >= 0 && static_cast<size_t>(id) < n.size()) ? n[static_cast<size_t>(id)].c_str() : "";
}

} // namespace visualmemory
