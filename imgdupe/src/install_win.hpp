// インストール先の登録（「アプリと機能」のアンインストール情報・ショートカット）。インストーラーと本体の両方で使う。
// ユーザーごとのインストール（HKCU、%LOCALAPPDATA%\Programs）なので管理者権限は要らない。
#pragma once

#include <string>

namespace install {

std::wstring defaultDir();        // %LOCALAPPDATA%\Programs\imgdupe
std::wstring installedDir();      // 前にインストールした場所（無ければ空）
void registerUninstall(const std::wstring& dir, const std::wstring& version, unsigned long estimatedKb);
void unregisterUninstall();
// 本体の起動時に呼ぶ: インストールした exe なら「アプリと機能」の版を今の版にする（自動更新の後も合うように）
void syncDisplayVersion(const std::wstring& exe, const std::wstring& version);

bool createShortcuts(const std::wstring& exe, bool startMenu, bool desktop);
void removeShortcuts();

}  // namespace install
