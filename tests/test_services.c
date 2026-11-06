/* System services contracts: dialogs (progress bars run until closed), PlayGo, trophies. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_services.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }
int gow3gpu_text_input_begin(const char *text, const char *prompt) { (void)text; (void)prompt; return 0; }
int gow3gpu_text_input_poll(char *text, uint64_t size) { (void)text; (void)size; return 0; }
/* trophy00.trp with 3 entries: a config and two trophy icons. */
static unsigned char trp[0x60+3*64];
int64_t runtime_file_open(const char *path, int flags, int mode) { (void)flags; (void)mode; return strstr(path,"trophy00.trp") ? 7 : -1; }
int64_t runtime_file_read(int fd, void *buffer, uint64_t size) { if (fd!=7) return -1; size=size<sizeof(trp) ? size : sizeof(trp); memcpy(buffer,trp,size); return (int64_t)size; }
const char *runtime_file_user_dir(void) { return "out"; }
static int notices;
void gow3gpu_notice(const char *text) { (void)text; ++notices; }
int64_t runtime_file_close(int fd) { (void)fd; return 0; }
static int choice_next=-1;
int gow3gpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus) { (void)title; (void)items; (void)images; (void)focus; return count>0; }
int gow3gpu_choice_poll(void) { return choice_next; }
int runtime_savedata_param(int32_t user, const char *dir, void *param) { (void)user; (void)dir; (void)param; return -1; }
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size) { (void)user; (void)dir; (void)out; (void)size; return -1; }
static int second_pad;
int runtime_pad_second_user(void) { return second_pad; }
static struct { int count; int32_t event[2]; } heard;
static ABI void heard_event(const int32_t *event, void *arg) {
    assert(arg==&heard);
    ++heard.count; heard.event[0]=event[0]; heard.event[1]=event[1];
}

int main(void) {
    unsigned char param[128]={0}, result[64]={0}; /* the game zeroes the pointers it does not want */
    uint32_t value;

    /* MsgDialog: a user message finishes with OK; a progress bar runs until closed. */
    assert(msg_init()==0);
    memset(param+56,0,4); param[56]=1;
    assert(msg_open(param)==0 && msg_status()==DIALOG_FINISHED);
    assert(msg_result(result)==0);
    memcpy(&value,result+4,4); assert(value==1);
    param[56]=2;
    assert(msg_open(param)==0 && msg_status()==DIALOG_RUNNING);
    assert(msg_result(result)==DIALOG_NOT_FINISHED);
    assert(msg_progress(0,50)==0 && msg_progress(1,50)==DIALOG_PARAM_INVALID);
    assert(msg_close()==0 && msg_status()==DIALOG_FINISHED && msg_close()==DIALOG_NOT_RUNNING);
    assert(msg_progress(0,100)==DIALOG_NOT_RUNNING);
    assert(msg_result(NULL)==DIALOG_ARG_NULL);
    assert(msg_term()==0 && msg_open(NULL)==DIALOG_ARG_NULL);

    /* SaveDataDialog: mode at offset 52 (5 = progress bar); result after the mode field. */
    assert(save_init()==0 && save_ready()==1);
    param[52]=5;
    assert(save_open(param)==0 && save_status()==DIALOG_RUNNING && save_progress(0,10)==0);
    assert(save_close()==0 && save_result(result)==0);
    memcpy(&value,result+8,4); assert(value==0); /* closed by the game: no button */
    param[52]=2;
    assert(save_open(param)==0 && save_status()==DIALOG_FINISHED && save_result(result)==0);
    memcpy(&value,result+8,4); assert(value==1);
    assert(save_term()==0);

    /* Save list: the player's choice comes back as the save directory. */
    char names[2][32]={"SAVE0001","SAVE0002"}, chosen[32];
    unsigned char items[64]={0}, list[128]={0}, out[64]={0};
    const char *names_ptr=names[0]; uint32_t name_count=2; const char *new_title="New";
    memcpy(items+16,&names_ptr,8); memcpy(items+24,&name_count,4); memcpy(items+32,&new_title,8);
    unsigned char *items_ptr=items; char *chosen_ptr=chosen;
    list[52]=1; list[56]=2; memcpy(list+72,&items_ptr,8);
    memcpy(out+16,&chosen_ptr,8);
    assert(save_init()==0 && save_open(list)==0 && save_status()==DIALOG_RUNNING);
    choice_next=2; /* entries: New save, SAVE0001, SAVE0002 */
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0 && !strcmp(chosen,"SAVE0002"));
    memcpy(&value,out+4,4); assert(value==0);
    memcpy(&value,out+8,4); assert(value==1);
    assert(save_open(list)==0);
    choice_next=0;
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0 && chosen[0]==0); /* new save */
    assert(save_open(list)==0);
    choice_next=-2;
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
    memcpy(&value,out+4,4); assert(value==SAVE_RESULT_CANCELED);
    assert(save_term()==0);

    /* PlayGo: installed, full speed, progress never divides by zero. */
    int32_t handle=0; uint64_t progress[2]; int32_t speed; uint16_t chunk=0;
    assert(playgo_init(NULL)==0 && playgo_open(&handle,NULL)==0);
    assert(playgo_get_speed(handle,&speed)==0 && speed==2);
    assert(playgo_progress(handle,&chunk,1,progress)==0 && progress[0]==progress[1] && progress[1]);
    assert(playgo_progress(handle,&chunk,0,progress)==PLAYGO_BAD_SIZE);
    assert(playgo_todo(handle,&chunk,1)==0 && playgo_todo(handle,NULL,1)==PLAYGO_BAD_POINTER);
    assert(playgo_close(handle)==0 && playgo_close(handle)==PLAYGO_BAD_HANDLE && playgo_terminate()==0);

    /* Trophies: none at first; an unlock is shown once and kept in out/trophies.txt. */
    remove("out/trophies.txt");
    trp[0x13]=3; strcpy((char *)trp+0x60,"TROPCONF.ESFM"); strcpy((char *)trp+0xa0,"TROP000.PNG"); strcpy((char *)trp+0xe0,"TROP001.PNG");
    uint32_t flags[4]={~0u,~0u,~0u,~0u}, count=7;
    assert(trophy_unlock_state(1,1,flags,&count)==0 && !flags[0] && !flags[3] && count==2);
    int32_t platinum=0;
    assert(trophy_unlock(1,1,33,&platinum)==0 && platinum==-1 && notices==1);
    assert(trophy_unlock(1,1,33,&platinum)==TROPHY_ALREADY_UNLOCKED && notices==1);
    assert(trophy_unlock(1,1,128,&platinum)==TROPHY_INVALID_ID);
    trophies_loaded=0; trophy_count=0; memset(trophy_flags,0,sizeof(trophy_flags)); /* a new start */
    assert(trophy_unlock_state(1,1,flags,&count)==0 && flags[1]==2u && !flags[0] && count==2);
    assert(trophy_unlock_state(1,1,NULL,&count)==TROPHY_INVALID && trophy_release(0)==TROPHY_INVALID);

    /* Users: 1 logs in at start; 2 logs in and out with the second gamepad (split-screen). */
    int32_t event[2], ids[4];
    char name[17];
    assert(user_event(event)==0 && event[0]==0 && event[1]==USER_ID);
    assert(user_event(event)==USER_NO_EVENT);
    assert(user_list(ids)==0 && ids[0]==USER_ID && ids[1]==-1);
    second_pad=1;
    assert(user_event(event)==0 && event[0]==0 && event[1]==USER2_ID);
    assert(user_event(event)==USER_NO_EVENT);
    assert(user_list(ids)==0 && ids[1]==USER2_ID);
    assert(user_name(USER2_ID,name,sizeof name)==0 && !strcmp(name,"Player 2"));
    second_pad=0;
    assert(user_event(event)==0 && event[0]==1 && event[1]==USER2_ID);
    assert(user_event(event)==USER_NO_EVENT);
    /* The callback hears the same, from player 1's pad reads (every 30th). */
    assert(user_register_callback(heard_event,&heard)==0);
    second_pad=1;
    for (int i=0;i<30;++i) runtime_user_events();
    assert(heard.count==1 && heard.event[0]==0 && heard.event[1]==USER2_ID);
    for (int i=0;i<60;++i) runtime_user_events();
    assert(heard.count==1);
    second_pad=0;
    for (int i=0;i<30;++i) runtime_user_events();
    assert(heard.count==2 && heard.event[0]==1);
    assert(user_unregister_callback()==0);

    float ratio=0; assert(safe_area(&ratio)==0 && ratio==1.0f && safe_area(NULL)==SYSTEM_PARAMETER);
    puts("services test: ok");
    return 0;
}
