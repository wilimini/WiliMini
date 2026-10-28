#ifndef PSP_VIDEO_SOURCE_IO_H
#define PSP_VIDEO_SOURCE_IO_H
static void source_memory(const char *phase) {
    const struct mallinfo heap = mallinfo();
    printf("MEM %s heap_used=%u system_free=%u system_max=%u\n", phase,
           (unsigned)heap.uordblks, sceKernelTotalFreeMemSize(), sceKernelMaxFreeMemSize());
}
typedef struct {
    AVIOContext *io;
    int64_t offset;
    const PspVideoSource *source;
} VideoSourceIO;
static int source_read(void *opaque,uint8_t *buffer,int bytes){
    VideoSourceIO *reader=opaque;
    if(quit_requested)return AVERROR_EXIT;
    if(!reader->source)return AVERROR(EIO);
    int ret=reader->source->read_at(reader->source->opaque,reader->offset,buffer,bytes);
    if(ret>0)reader->offset+=ret;
    return ret>0?ret:ret==0?AVERROR_EOF:AVERROR(EIO);
}
static int64_t source_seek(void *opaque,int64_t offset,int whence){
    VideoSourceIO *reader=opaque;
    if(!reader->source)return AVERROR(EIO);
    if(whence==AVSEEK_SIZE)return reader->source->size;
    whence&=~AVSEEK_FORCE;
    int64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?reader->offset:whence==SEEK_END?reader->source->size:-1;
    if(base<0 || offset < -base || offset>reader->source->size-base)return AVERROR(EINVAL);
    return reader->offset=base+offset;
}
static int source_open(AVFormatContext **format,const char *path,const PspVideoSource *source,VideoSourceIO *reader){
    memset(reader,0,sizeof(*reader));
    reader->source=source;
    if(!source)return avformat_open_input(format,path,NULL,NULL);
    *format=avformat_alloc_context();
    uint8_t *buffer=av_malloc(32768);
    if(!*format || !buffer){av_free(buffer);avformat_free_context(*format);*format=NULL;return AVERROR(ENOMEM);}
    reader->io=avio_alloc_context(buffer,32768,0,reader,source_read,NULL,source->size < 0 ? NULL : source_seek);
    if(!reader->io){av_free(buffer);avformat_free_context(*format);*format=NULL;return AVERROR(ENOMEM);}
    if(source->size < 0) {
        reader->io->seekable=0;
        (*format)->probesize=256*1024;
        (*format)->max_analyze_duration=500000;
    }
    (*format)->pb=reader->io;(*format)->flags|=AVFMT_FLAG_CUSTOM_IO;
    return avformat_open_input(format,NULL,NULL,NULL);
}
static void source_close(AVFormatContext **format,VideoSourceIO *reader){
    avformat_close_input(format);
    if(reader->io){av_freep(&reader->io->buffer);avio_context_free(&reader->io);}
}
static int64_t source_origin_us(const AVFormatContext *format) {
    return format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time;
}
static int source_start_at(AVFormatContext *format, int stream, int64_t start_ms) {
    if (start_ms <= 0) return 0;
    if (format->pb && !(format->pb->seekable & AVIO_SEEKABLE_NORMAL))
        return AVERROR(ENOSYS);
    const int64_t target = av_rescale_q(start_ms * 1000 + source_origin_us(format),
                                       AV_TIME_BASE_Q, format->streams[stream]->time_base);
    return av_seek_frame(format, stream, target, AVSEEK_FLAG_BACKWARD);
}
#endif
