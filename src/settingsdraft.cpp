#include "settingsdraft.h"

namespace app {

SettingsDraft::SettingsDraft(QObject *parent)
    : QObject(parent)
{
}

void SettingsDraft::loadFrom(const AppSettings &s)
{
    m_settings = s;
    emit changed();
}

void SettingsDraft::resetToDefaults()
{
    //   默认值的唯一来源仍是 `AppSettings` 的成员初值（不另写一份清单）
    m_settings = AppSettings{};
    emit changed();
}

}   // namespace app
