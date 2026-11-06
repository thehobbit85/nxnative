// Exercise both public feedback range bind encodings and the real
// DrawTransformFeedback producer/consumer ABI using the loader's shader key.
static void check_feedback_streams(NVNdevice *device, NVNqueue *queue, NVNcommandBuffer *cb,
                                   NVNbufferAddress addr, uint8_t *bytes) {
    enum { code=0x180, stream0=0x240, stream2=0x340, ctl=0x400, count=0x440 };
    uint8_t control[0x880] = {[0x704]=1};
    bytes[code]=11;
    memset(bytes+stream0,0xa5,0x200);
    memset(bytes+count,0,4);
    NVNshaderData shader={addr+code,control};
    NVNprogram program;
    assert(API(nvnProgramInitialize)(&program,device));
    assert(API(nvnProgramSetShaders)(&program,1,&shader));
    NVNbufferRange ranges[2]={{addr+stream0,6*20},{0,0}};
    API(nvnCommandBufferBeginRecording)(cb);
    API(nvnCommandBufferSetRenderTargets)(cb,0,NULL,NULL,NULL,NULL);
    API(nvnCommandBufferBindProgram)(cb,NULL,NVN_SHADER_STAGE_FRAGMENT_BIT);
    API(nvnCommandBufferBindProgram)(cb,&program,NVN_SHADER_STAGE_VERTEX_BIT);
    API(nvnCommandBufferBindStorageBuffer)(cb,NVN_SHADER_STAGE_VERTEX,3,addr+count,4);
    API(nvnCommandBufferBindTransformFeedbackBuffers)(cb,0,2,ranges);
    API(nvnCommandBufferBindTransformFeedbackBuffer)(cb,2,addr+stream2,5*8);
    API(nvnCommandBufferSetRasterizerDiscard)(cb,1);
    API(nvnCommandBufferBeginTransformFeedback)(cb,0);
    API(nvnCommandBufferDrawArrays)(cb,NVN_DRAW_PRIMITIVE_LINES,11,6);
    API(nvnCommandBufferSetRasterizerDiscard)(cb,0);
    API(nvnCommandBufferDrawArrays)(cb,NVN_DRAW_PRIMITIVE_POINTS,77,1);
    API(nvnCommandBufferEndTransformFeedback)(cb,addr+ctl);
    API(nvnCommandBufferSetRasterizerDiscard)(cb,1);
    API(nvnCommandBufferDrawTransformFeedback)(cb,NVN_DRAW_PRIMITIVE_POINTS,addr+ctl);
    API(nvnCommandBufferSetRasterizerDiscard)(cb,0);
    NVNcommandHandle handle=API(nvnCommandBufferEndRecording)(cb);
    API(nvnQueueSubmitCommands)(queue,1,&handle); API(nvnQueueFinish)(queue);
    const uint32_t state[8]={100,0,40,0,5,1,0,0};
    assert(!memcmp(bytes+ctl,state,sizeof state));
    for (unsigned k=0; k<5; k++) {
        uint32_t v=k==4 ? 77 : 11+k;
        const uint32_t *a=(const void *)(bytes+stream0+20*k), *b=(const void *)(bytes+stream2+8*k);
        assert(a[0]==v && a[1]==0 && a[2]==(0xf1230000u|v) && a[4]==v);
        assert(b[0]==v && b[1]==0);
    }
    assert(bytes[stream0+100]==0xa5 && bytes[stream2+40]==0xa5);
    if (*(uint32_t *)(bytes+count)!=12) die("feedback: got %u vertex invocations, expected 12",*(uint32_t *)(bytes+count));
    API(nvnProgramFinalize)(&program);
    puts("Public NVN feedback: range bind, whole lines, attachmentless points and draw of five saved vertices");
}
