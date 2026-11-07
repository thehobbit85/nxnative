static void global_loads(void) {
    mtl_queue_activate(1655);
    uint8_t data[512],second[256];uint32_t uniform[16384]={0},output[64],odd[9];uint64_t address=0;
    uint8_t *unaligned=(uint8_t *)odd+1;
    mtl_pool_addressed(unaligned,33);mtl_pool_add(unaligned,33);
    mtl_pool_add(data,sizeof data);mtl_pool_add(second,sizeof second);mtl_pool_add(data+64,64);
    mtl_pool_add(uniform,sizeof uniform);mtl_pool_add(output,sizeof output);mtl_pool_add(&address,sizeof address);
    mtl_pool_addressed(data,sizeof data);mtl_pool_addressed(second,sizeof second);
    uint64_t pointer=(uintptr_t)&address,result=(uintptr_t)output;
    memcpy(uniform,&pointer,8);memcpy(uniform+14,&result,8);
    const unsigned offsets[]={0,1,2,3,240,248,252,256,496,504,508,512};unsigned cases=0;
    for (unsigned variant=0;variant<7;variant++) {
        global_load_program[variant].nstage=1;
        unsigned width=variant<2 ? 1 : variant<4 ? 2 : 4,words=variant==5 ? 2 : variant==6 ? 4 : 1;
        for (unsigned test=0;test<18;test++) {
            unsigned offset=test<12 ? offsets[test] : 0;
            if (offset%width) continue;
            for (unsigned i=0;i<sizeof data;i++) data[i]=(uint8_t)(0x81+29*i);
            memset(odd,0x5b,sizeof odd);memset(second,0x37,sizeof second);memset(output,0xcc,sizeof output);
            address=test<12 ? (uintptr_t)data+offset : test==12 ? (uintptr_t)second : test==13 ? 0 : test==14 ? UINT64_C(0xfffffffffc) : test==15 ? (uintptr_t)data+64 : (uintptr_t)unaligned+(test==17 ? 3 : 0);
            if (address%width) continue;
            list_t list={0};
            if (test==15) {
                RECORD(OP_CLEAR_BUF,p_clear_buf,.addr=(uintptr_t)data+64,.size=16,.value=0x12345678);
            }
            RECORD(OP_BIND_UBO,p_bind_buf,.stage=5,.index=0,.addr=(uintptr_t)uniform,.size=sizeof uniform);
            RECORD(OP_BIND_PROGRAM,p_bind_program,.prog=&global_load_program[variant],.stages=0x20);
            RECORD(OP_DISPATCH,p_dispatch,.x=1,.y=1,.z=1);
            put(&list,OP_END,NULL,0);mtl_sub_begin();mtl_exec(list.bytes);assert(mtl_wait(mtl_sub_commit(),UINT64_MAX));
            for (unsigned word=0;word<64;word++) {
                uint32_t expected=0xcccccccc;
                if (word<words) {
                    uint64_t at=address+4*word;expected=0;
                    if (at>=(uintptr_t)data && at-(uintptr_t)data<=sizeof data-width) memcpy(&expected,data+(at-(uintptr_t)data),width);
                    else if (at>=(uintptr_t)second && at-(uintptr_t)second<=sizeof second-width) memcpy(&expected,second+(at-(uintptr_t)second),width);
                    else if (at>=(uintptr_t)unaligned && at-(uintptr_t)unaligned<=33-width) memcpy(&expected,unaligned+(at-(uintptr_t)unaligned),width);
                    if (variant==1) expected=(uint32_t)(int32_t)(int8_t)expected;
                    if (variant==3) expected=(uint32_t)(int32_t)(int16_t)expected;
                }
                if (output[word]!=expected) die("raw load variant%u case%u word%u returned %#x expected %#x",variant,test,word,output[word],expected);
            }
            cases++;
        }
    }
    mtl_pool_del(unaligned);mtl_pool_del(data+64);mtl_pool_del(data);mtl_pool_del(second);mtl_pool_del(uniform);mtl_pool_del(output);mtl_pool_del(&address);
    mtl_queue_forget(1655);
    printf("Raw global loads passed %u actual-Maxwell cases with signed widths, vector loads, missing addresses, overlapping/unaligned pools and prior GPU writes\n",cases);
}
