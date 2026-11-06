# Vendored Resilience OS API protos

`common.proto` and `rendered_output.proto` are copied unchanged from the BrightSign OS tree
(`meta-bs/recipes-bs/bs-resilience-proto/files/proto/brightsign/resilience/v1/`, package `brightsign.resilience.v1`).
They are the OS-owned contract for `RenderedOutputService`; the OS tree stays the source of truth. Only the
`argus_display_bridge` tool (optional, `-DENABLE_DISPLAY_BRIDGE=ON`) is generated from them. `libvisualmemory`,
`vm_api_server` and the capture pipeline do not depend on gRPC.

Do not edit these files here. To update them, copy the OS versions again.
