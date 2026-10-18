/* libSceJson2 (runtime_json.c) as WipEout uses it: build an object, serialize it, parse it back, read it. */
#include "runtime.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Stand-in for runtime.c: look up by symbol name, not NID. */
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(name,table[i].name)) return (uintptr_t)table[i].function;
    return 0;
}
typedef uintptr_t (ABI *Call)(void *, const void *, const void *);
typedef int32_t (ABI *Parse)(void *, const char *, uint64_t);
#define J(name) ((Call)runtime_json_resolve("_ZN" name))
#define JK(name) ((Call)runtime_json_resolve("_ZNK" name))
#define STR(handle) ((const char *)JK("3sce4Json6String5c_strEv")(handle,0,0))

int main(void) {
    void *key=0,*text=0,*root=0,*list=0,*item=0,*value=0,*parsed=0,*out=0,*it=0,*end=0;
    Parse parse=(Parse)runtime_json_resolve("_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm");
    J("3sce4Json6ObjectC1Ev")(&root,0,0);
    J("3sce4Json6StringC1EPKc")(&key,"name",0); J("3sce4Json6StringC1EPKc")(&text,"Ag \"Systems\"",0);
    J("3sce4Json5ValueC1ERKNS0_6StringE")(&value,&text,0);
    J("3sce4Json5ValueaSERKS1_")((void *)J("3sce4Json6ObjectixERKNS0_6StringE")(&root,&key,0),&value,0);
    J("3sce4Json5ValueD1Ev")(&value,0,0); J("3sce4Json6StringD1Ev")(&key,0,0);
    J("3sce4Json5ValueC1Em")(&value,(void *)(uintptr_t)7,0);
    J("3sce4Json5ArrayC1Ev")(&list,0,0); J("3sce4Json5Array9push_backERKNS0_5ValueE")(&list,&value,0);
    J("3sce4Json5ValueD1Ev")(&value,0,0);
    J("3sce4Json6StringC1EPKc")(&key,"laps",0);
    J("3sce4Json5ValueC1ERKNS0_5ArrayE")(&item,&list,0);
    J("3sce4Json5ValueaSERKS1_")((void *)J("3sce4Json6ObjectixERKNS0_6StringE")(&root,&key,0),&item,0);
    J("3sce4Json5ValueC1ERKNS0_6ObjectE")(&value,&root,0);
    J("3sce4Json6StringC1Ev")(&out,0,0);
    assert(J("3sce4Json5Value9serializeERNS0_6StringE")(&value,&out,0)==0);
    assert(!strcmp(STR(&out),"{\"name\":\"Ag \\\"Systems\\\"\",\"laps\":[7]}"));

    assert(parse(&parsed,STR(&out),strlen(STR(&out))+1)==0); /* a counted size may include the NUL */
    assert((int32_t)JK("3sce4Json5Value7getTypeEv")(&parsed,0,0)==7);
    void *object=(void *)JK("3sce4Json5Value9getObjectEv")(&parsed,0,0);
    JK("3sce4Json6Object4findERKNS0_6StringE")(&it,object,&key); JK("3sce4Json6Object3endEv")(&end,object,0);
    assert((uint8_t)JK("3sce4Json6Object8iteratorneERKS2_")(&it,&end,0));
    void *laps=(char *)JK("3sce4Json6Object8iteratorptEv")(&it,0,0)+0x10; /* the game reads it->second at +0x10 */
    JK("3sce4Json5Array5beginEv")(&it,(void *)JK("3sce4Json5Value8getArrayEv")(laps,0,0),0);
    assert(*(int64_t *)JK("3sce4Json5Value11getUIntegerEv")((void *)JK("3sce4Json5Array8iteratorptEv")(&it,0,0),0,0)==7);
    void *name=(void *)JK("3sce4Json5ValueixEPKc")(&parsed,"name",0);
    assert(!strcmp(STR((void *)JK("3sce4Json5Value9getStringEv")(name,0,0)),"Ag \"Systems\""));
    void *none=(void *)JK("3sce4Json5ValueixEPKc")(&parsed,"none",0); /* a missing member reads as null */
    assert((int32_t)JK("3sce4Json5Value7getTypeEv")(none,0,0)==0 && !*STR((void *)JK("3sce4Json5Value9getStringEv")(none,0,0)));

    assert(parse(&parsed,"{\"a\":",5)!=0);
    assert((int32_t)JK("3sce4Json5Value7getTypeEv")(&parsed,0,0)==7); /* a failed parse keeps the old value */
    const char *array=" [-2, 1.5e1, true, null, \"\\u00e9\\ud83d\\ude00\"] ";
    assert(parse(&parsed,array,strlen(array))==0);
    assert(*(int64_t *)JK("3sce4Json5Value10getIntegerEv")((void *)JK("3sce4Json5ValueixEm")(&parsed,0,0),0,0)==-2);
    assert(*(double *)JK("3sce4Json5Value7getRealEv")((void *)JK("3sce4Json5ValueixEm")(&parsed,(void *)1,0),0,0)==15.0);
    assert(*(uint8_t *)JK("3sce4Json5Value10getBooleanEv")((void *)JK("3sce4Json5ValueixEm")(&parsed,(void *)2,0),0,0)==1);
    assert(!strcmp(STR((void *)JK("3sce4Json5Value9getStringEv")((void *)JK("3sce4Json5ValueixEm")(&parsed,(void *)4,0),0,0)),
                   "\xc3\xa9\xf0\x9f\x98\x80"));
    assert(JK("3sce4Json5Value5countEv")(&parsed,0,0)==5);

    J("3sce4Json5ValueD1Ev")(&parsed,0,0); J("3sce4Json5ValueD1Ev")(&value,0,0); J("3sce4Json5ValueD1Ev")(&item,0,0);
    J("3sce4Json5ArrayD1Ev")(&list,0,0); J("3sce4Json6ObjectD1Ev")(&root,0,0);
    J("3sce4Json6StringD1Ev")(&out,0,0); J("3sce4Json6StringD1Ev")(&key,0,0); J("3sce4Json6StringD1Ev")(&text,0,0);
    assert(!root && !list && !value && !out && !parsed);
    puts("PASS: Json2 build, serialize, parse, read");
    return 0;
}
