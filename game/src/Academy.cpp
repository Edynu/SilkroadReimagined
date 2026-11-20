#include "sr/game/Academy.h"

namespace sr::game::academy {

const char* RefusalKey(std::uint16_t code) {
    switch (code) {
    case code::kInvalidTarget: return "UIIT_STT_ERR_COMMON_INVALID_TARGET";
    case code::kDead: return "UIIT_MSG_TC_ERROR_DIE_PASSING";
    case code::kLevel: return "UIIT_MSG_TC_ERROR_OPEN_LEVEL";
    case code::kGold: return "UIIT_MSG_TC_ERROR_OPEN_GOLD";
    case code::kNoAcademy: return "UIIT_MSG_TC_ERROR_TRAININGCAMP_NOTHING";
    case code::kAlreadyMember: return "UIIT_MSG_TC_ERROR_TRAININGCAMP_ALREADY_ENTRY";
    case code::kAssistantsFull: return "UIIT_MSG_TC_ERROR_SECOND_GUARDIAN_COUNT_OVER";
    case code::kRefused: return "UIIT_MSG_TC_ERROR_INVITE_REJECTION";
    case code::kOtherAcademy: return "UIIT_MSG_TC_ERROR_OTHER_TRAININGCAMP_ENTRY";
    case 0x6813:
    case 0x6814:
    case 0x6815:
    case 0x6816: return "UIIT_MSG_TC_ERROR_ORDER_WORK";
    case 0x6817: return "UIIT_MSG_GUILD_COMMON_NOTEXIST";
    case code::kGraduateLevel: return "UIIT_MSG_TC_ERROR_GRADUATION_LEVER";
    case code::kFoundWait: return "UIIT_MSG_TC_ERROR_NOT_OPEN_7D";
    case code::kDisbandWait: return "UIIT_MSG_TC_ERROR_NOT_DISORGANIZATION_7D";
    case code::kRejoinWait: return "UIIT_MSG_TC_ERROR_NOT_ENTRY_24T";
    case code::kRecruitWait: return "UIIT_MSG_TC_ERROR_NOT_INVITE_24T";
    case code::kBanWait: return "UIIT_MSG_TC_ERROR_NOT_BAN_24T";
    case code::kLeaveWait: return "UIIT_MSG_TC_ERROR_NOT_OUT_24T";
    case code::kForcedLevel: return "UIIT_MSG_TC_ERROR_POSIBLE_41L";
    case code::kJobChange: return "UIIT_MSG_TC_ERROR_JOB_DRESS";
    case code::kNotPosition: return "UIIT_MSG_TC_ERROR_NOT_POSITION";
    case 0x6825: return "UIIT_MSG_TC_ERROR_ALREADY_OPEN";
    case 0x6826: return "UIIT_MSG_TC_ERROR_NOT_INFORMATION";
    case code::kApprenticesFull: return "UIIT_MSG_TC_ERROR_STUDENT_COUNT_OVER";
    case 0x6829: return "UIIT_STT_TC_BAN_MACHING_ERROR";
    case 0x682A: return "UIIT_MSG_TC_ERROR_INVITE_TYPE_DIFFERENT";
    default: return "";
    }
}

} // namespace sr::game::academy
