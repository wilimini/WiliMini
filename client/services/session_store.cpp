#include "session_store.hpp"
#include "../api/wiliwili/bilibili/util/json.hpp"
#include "../bilibili.h"
#include <cerrno>
#include <cstdio>
#include <map>

namespace pspapp {
Result<Session> SessionStore::parseCookies(const std::string &cookies) {
    std::map<std::string, std::string> values;
    size_t begin = 0;
    while (begin < cookies.size()) {
        size_t end = cookies.find(';', begin);
        if (end == std::string::npos)
            end = cookies.size();
        size_t start = begin;
        while (start < end && cookies[start] == ' ')
            ++start;
        size_t eq = cookies.find('=', start);
        if (eq < end) {
            auto key = cookies.substr(start, eq - start),
                 value = cookies.substr(eq + 1, end - eq - 1);
            if (key == "SESSDATA" || key == "bili_jct" || key == "DedeUserID" ||
                key == "DedeUserID__ckMd5") {
                for (unsigned char ch : value)
                    if (ch <= 32 || ch >= 127 || ch == ';')
                        return Result<Session>::failure({ErrorDomain::Auth,
                                                         ErrorKind::InvalidResponse,
                                                         ErrorOperation::PollLogin});
                values[key] = std::move(value);
            }
        }
        begin = end + 1;
    }
    if (values["SESSDATA"].empty() || values["DedeUserID"].empty() ||
        values["DedeUserID"].find_first_not_of("0123456789") != std::string::npos)
        return Result<Session>::failure(
            {ErrorDomain::Auth, ErrorKind::InvalidResponse, ErrorOperation::PollLogin});
    Session session;
    session.mid = values["DedeUserID"];
    for (const auto &pair : values)
        if (!pair.second.empty()) {
            if (!session.cookies.empty())
                session.cookies += "; ";
            session.cookies += pair.first + "=" + pair.second;
        }
    if (session.cookies.size() > 3900)
        return Result<Session>::failure(
            {ErrorDomain::Auth, ErrorKind::InvalidResponse, ErrorOperation::PollLogin});
    return {std::move(session), {}};
}
Result<Session> SessionStore::load() {
    // Recover an interrupted replacement on memory sticks whose rename cannot overwrite.
    FILE *file = std::fopen(path_.c_str(), "rb");
    if (!file && errno == ENOENT) {
        std::rename((path_ + ".bak").c_str(), path_.c_str());
        file = std::fopen(path_.c_str(), "rb");
    }
    if (!file) {
        if (errno != ENOENT)
            return Result<Session>::failure(
                {ErrorDomain::Storage, ErrorKind::IoFailure, ErrorOperation::CheckSession, errno});
        current_ = {};
        bili_http_set_session("");
        return {};
    }
    char buffer[8193];
    size_t length = std::fread(buffer, 1, sizeof(buffer), file);
    bool failed = std::ferror(file);
    std::fclose(file);
    try {
        if (failed || length == sizeof(buffer))
            throw std::runtime_error("Invalid session file");
        auto root = nlohmann::json::parse(buffer, buffer + length);
        if (root.at("version").get<int>() != 1)
            throw std::runtime_error("Unknown session format");
        auto session = parseCookies(root.at("cookies").get<std::string>());
        if (!session)
            throw std::runtime_error("Invalid session cookies");
        current_ = session.value;
        bili_http_set_session(current_.cookies.c_str());
        return session;
    } catch (const std::exception &) {
        return Result<Session>::failure(
            {ErrorDomain::Storage, ErrorKind::InvalidResponse, ErrorOperation::CheckSession});
    }
}
AppError SessionStore::save(const Session &session) {
    auto checked = parseCookies(session.cookies);
    if (!checked)
        return checked.error;
    const std::string temp = path_ + ".part";
    const std::string body =
        nlohmann::json{{"version", 1}, {"cookies", checked.value.cookies}}.dump();
    FILE *file = std::fopen(temp.c_str(), "wb");
    if (!file)
        return {ErrorDomain::Storage, ErrorKind::IoFailure, ErrorOperation::SaveSession, errno};
    bool ok = std::fwrite(body.data(), 1, body.size(), file) == body.size();
    if (std::fflush(file))
        ok = false;
    if (std::fclose(file))
        ok = false;
    if (!ok) {
        int code = errno;
        std::remove(temp.c_str());
        return {ErrorDomain::Storage, ErrorKind::IoFailure, ErrorOperation::SaveSession, code};
    }
    if (std::rename(temp.c_str(), path_.c_str())) {
        const std::string backup = path_ + ".bak";
        std::remove(backup.c_str());
        if (std::rename(path_.c_str(), backup.c_str())) {
            int code = errno;
            std::remove(temp.c_str());
            return {ErrorDomain::Storage, ErrorKind::IoFailure, ErrorOperation::SaveSession, code};
        }
        if (std::rename(temp.c_str(), path_.c_str())) {
            int code = errno;
            std::rename(backup.c_str(), path_.c_str());
            std::remove(temp.c_str());
            return {ErrorDomain::Storage, ErrorKind::IoFailure, ErrorOperation::SaveSession, code};
        }
        std::remove(backup.c_str());
    }
    current_ = std::move(checked.value);
    bili_http_set_session(current_.cookies.c_str());
    return {};
}
AppError SessionStore::clear() {
    // Leave live credentials unchanged if persistent logout could not be completed.
    if (std::remove(path_.c_str()) && errno != ENOENT)
        return {ErrorDomain::Storage, ErrorKind::IoFailure, ErrorOperation::Logout, errno};
    if (std::remove((path_ + ".bak").c_str()) && errno != ENOENT)
        return {ErrorDomain::Storage, ErrorKind::IoFailure, ErrorOperation::Logout, errno};
    if (std::remove((path_ + ".part").c_str()) && errno != ENOENT)
        return {ErrorDomain::Storage, ErrorKind::IoFailure, ErrorOperation::Logout, errno};
    current_ = {};
    bili_http_set_session("");
    return {};
}
} // namespace pspapp
