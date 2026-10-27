#ifndef PSP_LAB_ME_DECODE_H
#define PSP_LAB_ME_DECODE_H
#ifndef PSP_ME_DECODE
#define PSP_ME_DECODE 0
#endif
#if PSP_ME_DECODE
struct H264QpelContext;
int me_decode_start(void);
int me_decode_stop(void);
void me_decode_qpel_init(struct H264QpelContext *context, int bit_depth);
#if PSP_ME_DECODE == 4
struct H264ChromaContext;
void me_decode_chroma_init(struct H264ChromaContext *context, int bit_depth);
extern unsigned me_pipeline_batches, me_pipeline_handoffs, me_pipeline_wait_us;
extern unsigned me_pipeline_max_batch, me_pipeline_fallback;
extern unsigned me_pipeline_failure_mb, me_pipeline_failure_pixel, me_pipeline_failure_values;
extern unsigned me_pipeline_filters, me_pipeline_filter_rows;
extern unsigned me_pipeline_trace[4];
#endif
extern unsigned me_decode_jobs, me_decode_blocks, me_decode_checked, me_decode_mismatches;
extern int me_decode_error;
#if PSP_ME_DECODE_RENDER
struct AVFrame;
int me_decode_render(const struct AVFrame *frame);
int me_decode_render_finish(void);
extern unsigned me_decode_render_submitted, me_decode_render_completed;
#endif
#endif
#endif
