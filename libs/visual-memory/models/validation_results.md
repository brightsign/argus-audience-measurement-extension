# Validation results

Dataset: 41 public base images x (6 transforms) = 287 images. Transforms: blur, bright+20, bright-20, crop90, flip, jpeg_q30.
Tools: {'numpy': '1.26.4', 'rknn-toolkit2': '2.3.0'}

## (a)-(c) correctness

| check | result |
|---|---|
| ORT vs torch cosine min/mean/max | 1.0000 / 1.0000 / 1.0000 (pass=True) |
| ORT vs torch max abs diff (L2-normed) | 2.83e-07 |
| identical image -> bitwise-identical embedding | True |
| L2 norm min/max | 0.9999999 / 1.0000001 (pass=True) |

## (d) cosine similarity (min / mean / max)

| group | n | min / mean / max |
|---|---|---|
| same image (identical) | - | 1.0000 |
| base vs own transforms (all) | 246 | 0.8061 / 0.9645 / 0.9989 |
| &nbsp;&nbsp;blur | 41 | 0.8061 / 0.8963 / 0.9989 |
| &nbsp;&nbsp;bright+20 | 41 | 0.9653 / 0.9915 / 0.9968 |
| &nbsp;&nbsp;bright-20 | 41 | 0.9805 / 0.9921 / 0.9967 |
| &nbsp;&nbsp;crop90 | 41 | 0.9266 / 0.9598 / 0.9848 |
| &nbsp;&nbsp;flip | 41 | 0.9356 / 0.9691 / 0.9888 |
| &nbsp;&nbsp;jpeg_q30 | 41 | 0.9559 / 0.9782 / 0.9986 |
| different images, base vs base | 1640 | 0.3399 / 0.5089 / 0.7803 |
| different images, base vs other-image transforms | 9840 | 0.3070 / 0.5047 / 0.7897 |

## (e) retrieval (ORT FP32), every image queried against all others excluding itself

| top-1 is same-group | top-5 contains same-group | top-5 precision |
|---|---|---|
| 1.000 | 1.000 | 1.000 |

## (f) invalid inputs (exception raised)

| case | exception |
|---|---|
| nan_float_image | TypeError |
| nan_tensor | ValueError |
| zero_by_zero | ValueError |
| wrong_dtype_float32 | TypeError |
| wrong_channels | ValueError |
| zero_vector_l2 | ValueError |

## (g) RKNN simulator (x86, rk3588 target config) vs ORT

| variant | cos min / mean / max | max abs diff | top-5 overlap vs ORT | top-1 same-group | top-5 any | top-5 precision |
|---|---|---|---|---|---|---|
| fp16 | 0.9999 / 1.0000 / 1.0000 | 0.0020 | 1.000 | 1.000 | 1.000 | 1.000 |
| int8 | 0.9186 / 0.9794 / 0.9942 | 0.0907 | 0.980 | 1.000 | 1.000 | 1.000 |

Note: simulator results approximate, not bit-exact with, NPU hardware (esp. INT8). Hardware numbers must be checked on device.
