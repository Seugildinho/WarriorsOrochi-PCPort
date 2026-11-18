#include "auto_save.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
// The game's message box. Open takes the text and the kind (1 asks yes or no);
// the two results are read every frame until they stop being "running".
constexpr uint32_t dialogOpenPc=0x2ad440u,questionResultPc=0x2ad430u,noticeResultPc=0x2ad3d0u;
constexpr int32_t dialogYes=2,noticeClosed=2;
// Text table read by the original at 0x23ec20.
constexpr uint32_t messageTable=0x331bc0u;
constexpr uint32_t msgAskSave=0xdc,msgChecking=0,msgCardHasSave=8,msgCardHasNoSave=9,msgSaving=0xe,msgSaved=0x13;
// The end-of-battle screens live in the overlay; the pause menu is in the ELF.
constexpr uint32_t overlayBase=0x819000u;
// How long "Data saved" stays up, in frames: long enough to be read.
constexpr int savedNoticeFrames=120;

PS2Runtime::RecompiledFunction dialogOpenOriginal{},questionResultOriginal{},noticeResultOriginal{};
bool anyCaller=false;
bool saving=false,answerYes=false;
int noticeFrames=0;

bool isMessage(uint8_t *ram,PS2Runtime *runtime,uint32_t text,uint32_t message) {
    const auto *shown=reinterpret_cast<const char *>(getMemPtr(ram,text));
    const auto *wanted=reinterpret_cast<const char *>(getMemPtr(ram,runtime->memory().read32(messageTable+message*4)));
    // Questions are built as this message plus a second line.
    return shown && wanted && std::strncmp(shown,wanted,std::strlen(wanted))==0;
}

// One copy of the save it is about to replace, outside the card's own folder.
void backUpCard() {
    const auto card=PS2Runtime::getIoPaths().mcRoot;
    const auto backup=card.parent_path()/"mc0-backup";
    std::error_code error;
    std::filesystem::remove_all(backup,error);
    std::filesystem::copy(card,backup,std::filesystem::copy_options::recursive,error);
    if(error) std::cerr << "[Orochi:autosave] Backup failed: " << error.message() << '\n';
}

void openDialog(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    const auto text=getRegU32(ctx,4);
    if(!saving) {
        if(isMessage(ram,runtime,text,msgAskSave) && (anyCaller || getRegU32(ctx,31)>=overlayBase)) {
            saving=answerYes=true;
            backUpCard();
            std::cout << "[Orochi:autosave] Saving.\n";
        }
    } else if(isMessage(ram,runtime,text,msgCardHasSave) || isMessage(ram,runtime,text,msgCardHasNoSave)) {
        answerYes=true;
    } else if(isMessage(ram,runtime,text,msgSaved)) {
        noticeFrames=savedNoticeFrames;
        std::cout << "[Orochi:autosave] Saved.\n";
    } else if(!isMessage(ram,runtime,text,msgChecking) && !isMessage(ram,runtime,text,msgSaving)) {
        saving=answerYes=false;
        std::cout << "[Orochi:autosave] Stopped: the game needs the player here.\n";
    }
    dialogOpenOriginal(ram,ctx,runtime);
}

void readQuestion(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    if(!answerYes) {
        questionResultOriginal(ram,ctx,runtime);
        return;
    }
    answerYes=false;
    setReturnS32(ctx,dialogYes);
    ctx->pc=getRegU32(ctx,31);
}

void readNotice(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    if(noticeFrames==0 || --noticeFrames>0) {
        noticeResultOriginal(ram,ctx,runtime);
        return;
    }
    saving=false;
    setReturnS32(ctx,noticeClosed);
    ctx->pc=getRegU32(ctx,31);
}
}

bool installOrochiAutoSave(PS2Runtime &runtime) {
    const char *mode=std::getenv("OROCHI_AUTOSAVE");
    if(!mode || std::string(mode)=="0") return true;
    anyCaller=std::string(mode)=="all";
    dialogOpenOriginal=runtime.lookupFunction(dialogOpenPc);
    questionResultOriginal=runtime.lookupFunction(questionResultPc);
    noticeResultOriginal=runtime.lookupFunction(noticeResultPc);
    return dialogOpenOriginal && questionResultOriginal && noticeResultOriginal &&
        runtime.replaceFunction(dialogOpenPc,openDialog) && runtime.replaceFunction(questionResultPc,readQuestion) &&
        runtime.replaceFunction(noticeResultPc,readNotice);
}
